/** /proc/kerrighed/ manager
 *  @file proc.c
 *
 *  Copyright (C) 2001-2006, INRIA, Universite de Rennes 1, EDF.
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 */

#include <asm/processor.h>
#include <linux/kernel_stat.h>
#include <linux/seq_file.h>
#include <linux/if.h>
#include <asm/div64.h>
#include <linux/threads.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <kerrighed/krgflags.h>
#include <kerrighed/procfs.h>

#define MODULE_NAME "Distributed /proc    "
#include "debug_procfs.h"

#ifndef FILE_NONE
#  if defined(FILE_NAZGUL_PROC) || defined(FILE_ALL)
#     define DEBUG_THIS_MODULE
#  endif
#endif

#include <tools/debug.h>
#include <tools/procfs.h>

#include <hotplug/hotplug.h>
#include "proc.h"
#include "static_node_info_linker.h"
#include "static_cpu_info_linker.h"
#include "dynamic_node_info_linker.h"
#include "dynamic_cpu_info_linker.h"
#ifdef CONFIG_KRG_SCHED_MOSIX_PROBE
#include "scheduler_old/mosix_probe.h"
#endif

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

/* /proc/kerrighed entries */

static struct proc_dir_entry *procfs_nodes;	/* /proc/kerrighed/nodes   */
static struct proc_dir_entry *procfs_nrnodes;	/* /proc/kerrighed/nodes/nrnodes */

void krg_create_seq_entry(char *name,
			  mode_t mode,
			  struct file_operations *f,
			  struct proc_dir_entry *parent, void *data)
{
	struct proc_dir_entry *entry;
	entry = create_proc_entry(name, mode, parent);
	if (entry) {
		entry->proc_fops = f;
		entry->data = data;
	}
}

/** Check if a < b */
static __inline__ int timespec_lt(struct timespec *a, struct timespec *b)
{
	return ((a->tv_sec < b->tv_sec) ||
		((a->tv_sec == b->tv_sec) && (a->tv_nsec < b->tv_nsec)));
}

static inline kerrighed_node_t get_req_node(kerrighed_node_t nodeid)
{
#ifdef CONFIG_KRG_CAP
	if (nodeid == PROC_STAT_DEPEND_ON_CAPABILITY) {
		if (cap_raised
		    (current->krg_cap_effective, CAP_SEE_LOCAL_PROC_STAT))
			return kerrighed_node_id;
		else
			return KERRIGHED_MAX_NODES;
	}
#endif
	return nodeid;
}


static inline krgnodemask_t get_proc_nodes_vector(kerrighed_node_t nodeid)
{
	krgnodemask_t nodes;
	nodeid = get_req_node(nodeid);
	krgnodes_clear(nodes);

	if (nodeid == KERRIGHED_MAX_NODES) {
		if (IS_KERRIGHED_NODE(KRGFLAGS_RUNNING))
			krgnodes_copy(nodes, krgnode_online_map);
		else
			krgnode_set(kerrighed_node_id, nodes);
	} else
		krgnode_set(nodeid, nodes);

	return nodes;
}

static void free_stat_buf(int size, void * buf)
{
	if (size > PAGE_SIZE)
		vfree(buf);
	else
		kfree(buf);
}

static int krg_proc_stat_open(struct inode *inode,
			      struct file *file,
			      int (*show) (struct seq_file *, void *), int size)
{
	char *buf;
	struct seq_file *m;
	int res;

	if (size > PAGE_SIZE)
		buf = vmalloc(size);
	else
		buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	res = single_open(file, show, PROC_I(inode)->krg_procfs_private);
	if (!res) {
		m = file->private_data;
		m->buf = buf;
		m->size = size;
	} else
		free_stat_buf(size, buf);

	return res;
}



static int krg_proc_stat_release (struct inode *inode,
				  struct file *file)
{
	struct seq_file *m = file->private_data;

	free_stat_buf(m->size, m->buf);
	m->buf = NULL;

	return single_release (inode, file);
}



/****************************************************************************/
/*                                                                          */
/*                   /proc/kerrighed/cpuinfo  Management                    */
/*                                                                          */
/****************************************************************************/

struct cpu_info_seq_struct {
	int cur_node;
	int last_node;
	int cpu_id;
	int req_node;
	int last_pos;
};

static void init_cpu_info_seq_struct(struct cpu_info_seq_struct *seq_data)
{
	kerrighed_node_t req_node ;

	req_node = get_req_node(seq_data->req_node);

	if (IS_KERRIGHED_NODE(KRGFLAGS_RUNNING)) {
		// Init values to parse CPU.
		if (req_node == KERRIGHED_MAX_NODES) {
			// Cluster wide CPU info
			seq_data->cur_node = nth_online_krgnode(0);
			seq_data->last_node = KERRIGHED_MAX_NODES - 1;
		} else {
			// Node wide CPU info
			seq_data->cur_node = req_node;
			seq_data->last_node = req_node;
		}
	} else {
		seq_data->cur_node = kerrighed_node_id;
		seq_data->last_node = kerrighed_node_id;
	}
	seq_data->cpu_id = 0;
	seq_data->last_pos = 0;
}

static void go_to_selected_cpu(struct cpu_info_seq_struct *seq_data,
			       loff_t pos)
{
	krg_static_node_info_t *static_node_info;
	int i;

	for (i = seq_data->last_pos; i < pos; i++) {
		seq_data->cpu_id++;
		static_node_info = get_static_node_info(seq_data->cur_node);
		if (seq_data->cpu_id >= static_node_info->nr_cpu) {
			seq_data->cur_node =
				krgnode_next_online(seq_data->cur_node);
			seq_data->cpu_id = 0;
		}
	}
	seq_data->last_pos = pos;
}

static void *c_start(struct seq_file *m, loff_t * pos)
{
	struct cpu_info_seq_struct *seq_data = m->private;
	krg_static_cpu_info_t *cpu_info;

	if (*pos == 0)
		init_cpu_info_seq_struct (seq_data);
	else {
		// Switch to the requested CPU.
		if (unlikely(*pos < seq_data->last_pos))
			init_cpu_info_seq_struct (seq_data);

		go_to_selected_cpu(seq_data, *pos);
	}

	if (seq_data->cur_node <= seq_data->last_node) {
		cpu_info =
		    get_static_cpu_info(seq_data->cur_node, seq_data->cpu_id);
		return &cpu_info->info;
	} else
		return NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t * pos)
{
	++*pos;

	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

struct seq_operations krg_cpuinfo_op = {
	.start = c_start,
	.next = c_next,
	.stop = c_stop,
	.show = show_cpuinfo,
};

extern struct seq_operations krg_cpuinfo_op;

static int krg_cpuinfo_open(struct inode *inode, struct file *file)
{
	struct cpu_info_seq_struct *seq_data;
	struct seq_file *m;
	int ret;

	ret = seq_open(file, &krg_cpuinfo_op);
	if (ret < 0)
		return ret;

	seq_data = kmalloc(sizeof(struct cpu_info_seq_struct), GFP_KERNEL);
	if (seq_data == NULL) {
		seq_release(inode, file);
		return -ENOMEM;
	}

	seq_data->req_node = (long)PROC_I(inode)->krg_procfs_private;
	m = file->private_data;
	m->private = seq_data;

	return 0;
}

static int krg_cpuinfo_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	kfree(m->private);

	return seq_release(inode, file);
}

static struct file_operations proc_krg_cpuinfo_operations = {
	.open = krg_cpuinfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_cpuinfo_release,
};

/****************************************************************************/
/*                                                                          */
/*                   /proc/kerrighed/meminfo  Management                    */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/kerrighed/meminfo entry.
 *  @author Renaud Lottiaux
 */
static int show_meminfo(struct seq_file *p, void *v)
{
	kerrighed_node_t nodeid = (long) p->private;
	krgnodemask_t nodes;
	krg_dynamic_node_info_t global_dyn_info;
	krg_dynamic_node_info_t *dyn_info;
	kerrighed_node_t node;
	long cached;
	
	nodes = get_proc_nodes_vector(nodeid);

	memset(&global_dyn_info, 0, sizeof(krg_dynamic_node_info_t));

	for_each_krgnode_mask(node, nodes) {
		dyn_info = get_dynamic_node_info(node);

		global_dyn_info.totalram += dyn_info->totalram;
		global_dyn_info.freeram += dyn_info->freeram;
		global_dyn_info.bufferram += dyn_info->bufferram;
		global_dyn_info.totalhigh += dyn_info->totalhigh;
		global_dyn_info.freehigh += dyn_info->freehigh;
		global_dyn_info.totalswap += dyn_info->totalswap;
		global_dyn_info.freeswap += dyn_info->freeswap;
		global_dyn_info.swapcache_pages += dyn_info->swapcache_pages;

		global_dyn_info.nr_file_pages += dyn_info->nr_file_pages;
		global_dyn_info.nr_file_dirty += dyn_info->nr_file_dirty;
		global_dyn_info.nr_writeback += dyn_info->nr_writeback;
		global_dyn_info.nr_anon_pages += dyn_info->nr_anon_pages;
		global_dyn_info.nr_file_mapped += dyn_info->nr_file_mapped;
		global_dyn_info.nr_page_table_pages +=
		    dyn_info->nr_page_table_pages;
		global_dyn_info.nr_slab_reclaimable +=
			dyn_info->nr_slab_reclaimable;
		global_dyn_info.nr_slab_unreclaimable +=
			dyn_info->nr_slab_unreclaimable;
		global_dyn_info.nr_unstable_nfs += dyn_info->nr_unstable_nfs;
		global_dyn_info.nr_bounce += dyn_info->nr_bounce;

		global_dyn_info.vmi.used += dyn_info->vmi.used;
		if (dyn_info->vmi.largest_chunk >
		    global_dyn_info.vmi.largest_chunk)
			global_dyn_info.vmi.largest_chunk =
			    dyn_info->vmi.largest_chunk;
		global_dyn_info.vmalloc_total += dyn_info->vmalloc_total;

		global_dyn_info.active += dyn_info->active;
		global_dyn_info.inactive += dyn_info->inactive;
		global_dyn_info.allowed += dyn_info->allowed;
		global_dyn_info.commited += dyn_info->commited;
	}

#define K(x) ((x) << (PAGE_SHIFT - 10))

        cached = global_dyn_info.nr_file_pages -
		global_dyn_info.swapcache_pages - global_dyn_info.bufferram;
	if (cached < 0)
		cached = 0;

	seq_printf(p,
		   "MemTotal:     %8lu kB\n"
		   "MemFree:      %8lu kB\n"
		   "Buffers:      %8lu kB\n"
		   "Cached:       %8lu kB\n"
		   "SwapCached:   %8lu kB\n"
		   "Active:       %8lu kB\n"
		   "Inactive:     %8lu kB\n"
#ifdef CONFIG_HIGHMEM
		   "HighTotal:    %8lu kB\n"
		   "HighFree:     %8lu kB\n"
		   "LowTotal:     %8lu kB\n"
		   "LowFree:      %8lu kB\n"
#endif
		   "SwapTotal:    %8lu kB\n"
		   "SwapFree:     %8lu kB\n"
		   "Dirty:        %8lu kB\n"
		   "Writeback:    %8lu kB\n"
		   "AnonPages:    %8lu kB\n"
		   "Mapped:       %8lu kB\n"
		   "Slab:         %8lu kB\n"
		   "SReclaimable: %8lu kB\n"
		   "SUnreclaim:   %8lu kB\n"
		   "PageTables:   %8lu kB\n"
		   "NFS_Unstable: %8lu kB\n"
		   "Bounce:       %8lu kB\n"
		   "CommitLimit:  %8lu kB\n"
		   "Committed_AS: %8lu kB\n"
		   "VmallocTotal: %8lu kB\n"
		   "VmallocUsed:  %8lu kB\n"
		   "VmallocChunk: %8lu kB\n",
		   K(global_dyn_info.totalram),
		   K(global_dyn_info.freeram),
		   K(global_dyn_info.bufferram),
		   K(cached),
		   K(global_dyn_info.swapcache_pages),
		   K(global_dyn_info.active),
		   K(global_dyn_info.inactive),

 #ifdef CONFIG_HIGHMEM
		   K(global_dyn_info.totalhigh),
		   K(global_dyn_info.freehigh),
		   K(global_dyn_info.totalram - global_dyn_info.totalhigh),
		   K(global_dyn_info.freeram - global_dyn_info.freehigh),
 #endif
		   K(global_dyn_info.totalswap),
		   K(global_dyn_info.freeswap),
		   K(global_dyn_info.nr_file_dirty),
		   K(global_dyn_info.nr_writeback),
		   K(global_dyn_info.nr_anon_pages),
		   K(global_dyn_info.nr_file_mapped),
		   K(global_dyn_info.nr_slab_reclaimable +
		     global_dyn_info.nr_slab_unreclaimable),
		   K(global_dyn_info.nr_slab_reclaimable),
		   K(global_dyn_info.nr_slab_unreclaimable),
		   K(global_dyn_info.nr_page_table_pages),
		   K(global_dyn_info.nr_unstable_nfs),
		   K(global_dyn_info.nr_bounce),
		   K(global_dyn_info.allowed),
		   K(global_dyn_info.commited),
		   global_dyn_info.vmalloc_total >> 10,
		   global_dyn_info.vmi.used >> 10,
		   global_dyn_info.vmi.largest_chunk >> 10);

	return 0;
}

static int meminfo_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_meminfo, 1500);
}

static struct file_operations proc_krg_meminfo_operations = {
	.open = meminfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/****************************************************************************/
/*                                                                          */
/*                     /proc/kerrighed/stat  Management                     */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/kerrighed/stat entry.
 *  @author Renaud Lottiaux
 *
 */
static int show_stat(struct seq_file *p, void *v)
{
	kerrighed_node_t req_nodeid = (long) p->private;
	krg_dynamic_cpu_info_t *dynamic_cpu_info;
	krg_dynamic_node_info_t *dynamic_node_info;
	krg_static_node_info_t *static_node_info;
	int i;
	krgnodemask_t nodes;
	cputime64_t user, nice, system, idle, iowait, irq, softirq, steal;
	unsigned long long nr_context_switches = 0;
	unsigned long jif = 0, total_forks = 0, nr_running = 0, nr_iowait = 0;
	kerrighed_node_t node_id;
	u64 total_intr = 0;
	int head_len;

	nodes = get_proc_nodes_vector(req_nodeid);

	user = nice = system = idle = iowait = irq = softirq = steal =
	    cputime64_zero;

	/* Keep space to overwrite "cpu" line later in order to get this line
	 * first without parsing data twice... Yes... Dirty...
	 */
	seq_printf(p,
		   "                                                            \n");
	for_each_krgnode_mask(node_id, nodes) {
		static_node_info = get_static_node_info(node_id);
		dynamic_node_info = get_dynamic_node_info(node_id);

		/* Compute node level stat informations */

		nr_context_switches += dynamic_node_info->nr_context_switches;
		if (dynamic_node_info->jif > jif)
			jif = dynamic_node_info->jif;
		total_forks += dynamic_node_info->total_forks;
		nr_running += dynamic_node_info->nr_running;
		nr_iowait += dynamic_node_info->nr_iowait;

		for (i = 0; i < static_node_info->nr_cpu; i++) {
			dynamic_cpu_info = get_dynamic_cpu_info(node_id, i);

			seq_printf(p,
				   "cpu%d  %llu %llu %llu %llu %llu %llu %llu %llu\n",
				   __krg_cpu_id(node_id, i),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->user),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->nice),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->
							system),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->idle),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->
							iowait),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->irq),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->
							softirq),
				   (unsigned long long)
				   cputime64_to_clock_t(dynamic_cpu_info->
							steal));

			/* Compute CPU level stat informations */

			user = cputime64_add(user, dynamic_cpu_info->user);
			nice = cputime64_add(nice, dynamic_cpu_info->nice);
			system =
			    cputime64_add(system, dynamic_cpu_info->system);
			idle = cputime64_add(idle, dynamic_cpu_info->idle);
			iowait =
			    cputime64_add(iowait, dynamic_cpu_info->iowait);
			irq = cputime64_add(irq, dynamic_cpu_info->irq);
			softirq =
			    cputime64_add(softirq, dynamic_cpu_info->softirq);
			steal = cputime64_add(steal, dynamic_cpu_info->steal);
			total_intr += dynamic_cpu_info->total_intr;
		}
	}

	/* Dirty trick to print "cpu" line at the beginning without parsing data
	 * twice
	 */
	head_len = snprintf(p->buf, 61,
			    "cpu   %llu %llu %llu %llu %llu %llu %llu %llu",
			    (unsigned long long)cputime64_to_clock_t(user),
			    (unsigned long long)cputime64_to_clock_t(nice),
			    (unsigned long long)cputime64_to_clock_t(system),
			    (unsigned long long)cputime64_to_clock_t(idle),
			    (unsigned long long)cputime64_to_clock_t(iowait),
			    (unsigned long long)cputime64_to_clock_t(irq),
			    (unsigned long long)cputime64_to_clock_t(softirq),
			    (unsigned long long)cputime64_to_clock_t(steal));
	/* The NUL byte inserted overwrote a blank...
	 * in the middle of the file! */
	if (head_len >= 60)
		/* NUL byte overwrote \n */
		p->buf[60] = '\n';
	else
		p->buf[head_len] = ' ';

	seq_printf(p, "intr %llu", (unsigned long long)total_intr);

	seq_printf(p,
		   "\nctxt %llu\n"
		   "btime %lu\n"
		   "processes %lu\n"
		   "procs_running %lu\n"
		   "procs_blocked %lu\n",
		   nr_context_switches,
		   jif, total_forks, nr_running, nr_iowait);

	return 0;
}

static int stat_open(struct inode *inode, struct file *file)
{
	unsigned size;

	size = 80 * (KERRIGHED_MAX_CPU_PER_NODE * kerrighed_nb_nodes + 6);

	return krg_proc_stat_open(inode, file, show_stat, size);
}

static struct file_operations proc_krg_stat_operations = {
	.open = stat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_proc_stat_release,
};

/****************************************************************************/
/*                                                                          */
/*                    /proc/kerrighed/loadavg  Management                   */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/kerrighed/loadavg entry.
 *  @author Renaud Lottiaux
 *
 */
static int show_loadavg(struct seq_file *p, void *v)
{
	kerrighed_node_t nodeid = (long) p->private;
	krg_dynamic_node_info_t *dynamic_node_info;
	krgnodemask_t nodes;
	kerrighed_node_t i;
	int a, b, c, nr_threads, last_pid;
	long nr_running;

	a = b = c = nr_running = nr_threads = last_pid = 0;

	nodes = get_proc_nodes_vector(nodeid);

	for_each_krgnode_mask(i, nodes) {
		dynamic_node_info = get_dynamic_node_info(i);
		a += dynamic_node_info->avenrun[0] + (FIXED_1 / 200);
		b += dynamic_node_info->avenrun[1] + (FIXED_1 / 200);
		c += dynamic_node_info->avenrun[2] + (FIXED_1 / 200);

		nr_running += dynamic_node_info->nr_running;
		nr_threads += dynamic_node_info->nr_threads;
		last_pid = dynamic_node_info->last_pid;
	}

	seq_printf(p, "%d.%02d %d.%02d %d.%02d %ld/%d %d\n",
		   LOAD_INT(a), LOAD_FRAC(a), LOAD_INT(b), LOAD_FRAC(b),
		   LOAD_INT(c), LOAD_FRAC(c), nr_running, nr_threads, last_pid);

	return 0;
}

static int loadavg_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_loadavg, 100);
}

static struct file_operations proc_krg_loadavg_operations = {
	.open = loadavg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_proc_stat_release,
};

/****************************************************************************/
/*                                                                          */
/*                    /proc/kerrighed/mosix_load  Management                */
/*                                                                          */
/****************************************************************************/



#ifdef CONFIG_KRG_SCHED_MOSIX_PROBE
/** Read function for /proc/kerrighed/mosix_load entry.
 *  @author Louis Rilling
 *
 */
static int show_mosix_load(struct seq_file *p, void *v)
{
	kerrighed_node_t nodeid = (long) p->private;
	krg_dynamic_node_info_t *dynamic_node_info;
	unsigned long norm_load, norm_process_load;
	kerrighed_node_t i;
	krgnodemask_t nodes;

	norm_load = norm_process_load = 0;

	nodes = get_proc_nodes_vector(nodeid);

	for_each_krgnode_mask(i, nodes) {
		dynamic_node_info = get_dynamic_node_info(i);
		norm_load += dynamic_node_info->mosix_load;
		if (krgnodes_weight(nodes) == 1)
			norm_process_load =
				dynamic_node_info->mosix_single_process_load;
	}

	seq_printf(p,
		   "normalized load: %lu\n"
		   "normalized single process load: ",
		   norm_load);

	if (krgnodes_weight(nodes) == 1)
		seq_printf(p, "%lu", norm_process_load);
	else
		seq_printf(p, "-");
	seq_printf(p, "\n");
	
	if (krgnode_is_unique(kerrighed_node_id, nodes))
		seq_printf(p,
			   "raw mean load: %lu\n"
			   "raw upper load: %lu\n"
			   "raw single process load: %lu\n",
			   mosix_mean_load,
			   mosix_upper_load,
			   mosix_single_process_load);
	else
		seq_printf(p,
			   "raw mean load: -\n"
			   "raw upper load: -\n"
			   "raw single process load: -\n");

	return 0;
}

static int mosix_load_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_mosix_load, 100);
}

static struct file_operations proc_krg_mosix_load_operations = {
	.open           = mosix_load_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = krg_proc_stat_release,
};
#endif



/****************************************************************************/
/*                                                                          */
/*                    /proc/kerrighed/nodeid  Management                    */
/*                                                                          */
/****************************************************************************/

static int show_nodeid(struct seq_file *p, void *v)
{
	kerrighed_node_t nodeid = (long)p->private;

	if (nodeid >= KERRIGHED_MAX_NODES)
		seq_printf(p, "-\n");
	else
		seq_printf(p, "%d\n", nodeid);

	return 0;
}

static int nodeid_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_nodeid, 10);
}

static struct file_operations proc_krg_nodeid_operations = {
	.open = nodeid_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_proc_stat_release,
};

/****************************************************************************/
/*                                                                          */
/*                 /proc/nodes/nrnodes  Management                          */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/nodes/nrnodes entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int krg_nrnodes_read_proc(char *buffer, char **start, off_t offset,
			  int count, int *eof, void *data)
{
	static char mybuffer[64];
	static int len;

	if (offset == 0)
		len = snprintf(mybuffer, 40, 
			       "POSSIBLE:%d\n"
			       "ONLINE:%d\n"
			       "PRESENT:%d\n",
			       num_possible_krgnodes(),
			       num_online_krgnodes(), 
			       num_present_krgnodes());

	if (offset + count >= len) {
		count = len - offset;
		if (count < 0)
			count = 0;
		*eof = 1;
	}

	memcpy(buffer, &mybuffer[offset], count);

	return count;
}

/****************************************************************************/
/*                                                                          */
/*                    /proc/kerrighed/session  Management                   */
/*                                                                          */
/****************************************************************************/

static int show_session(struct seq_file *p, void *v)
{
	seq_printf(p, "%d\n", kerrighed_session_id);

	return 0;
}

static int session_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_session, 10);
}

static struct file_operations proc_krg_session_operations = {
	.open = session_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_proc_stat_release,
};

/****************************************************************************/
/*                                                                          */
/*                    /proc/kerrighed/uptime  Management                    */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/kerrighed/uptime entry.
 *  @author Renaud Lottiaux
 */
static int show_uptime(struct seq_file *p, void *v)
{
	kerrighed_node_t nodeid = (long) p->private;
	krg_dynamic_node_info_t *dynamic_node_info;
	kerrighed_node_t i, nr_nodes = 0;
	krgnodemask_t nodes;
	struct timespec uptime;
	unsigned long long idle = 0;
	unsigned long idle_mod;

	nodes = get_proc_nodes_vector(nodeid);

	uptime.tv_sec = uptime.tv_nsec = 0;

	for_each_krgnode_mask(i, nodes) {
		dynamic_node_info = get_dynamic_node_info(i);
		nr_nodes++;

		if (timespec_lt(&uptime, &dynamic_node_info->uptime))
			uptime = dynamic_node_info->uptime;

		idle += (unsigned long long)dynamic_node_info->idletime.tv_sec *
		    NSEC_PER_SEC + dynamic_node_info->idletime.tv_nsec;
	}

	do_div(idle, nr_nodes);
	idle_mod = do_div(idle, NSEC_PER_SEC);

	seq_printf(p, "%lu.%02lu %lu.%02lu\n",
		   (unsigned long)uptime.tv_sec,
		   (uptime.tv_nsec / (NSEC_PER_SEC / 100)),
		   (unsigned long)idle, (idle_mod / (NSEC_PER_SEC / 100)));

	return 0;
}

static int uptime_open(struct inode *inode, struct file *file)
{
	return krg_proc_stat_open(inode, file, show_uptime, 100);
}

static struct file_operations proc_krg_uptime_operations = {
	.open = uptime_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = krg_proc_stat_release,
};

/****************************************************************************/
/*                                                                          */
/*                   /proc/kerrighed/net/dev  Management                    */
/*                                                                          */
/****************************************************************************/

/** Read function for /proc/kerrighed/nodeid entry.
 *  @author Renaud Lottiaux
 *
 *  @param buffer           Buffer to write data to.
 *  @param buffer_location  Alternative buffer to return...
 *  @param offset           Offset of the first byte to write in the buffer.
 *  @param buffer_length    Length of given buffer
 *
 *  @return  Number of bytes written.
 */
int krg_netdev_read_proc(char *buffer,
			 char **start,
			 off_t offset, int count, int *eof, void *data)
{
	return count;
}

/** Create a /proc/nodes/node<x> directory and sub-files.
 *  @author Renaud Lottiaux
 *
 *  @param nodeid   Id of the node to create a proc entry for.
 *
 *  @return  0 if everything ok.
 *           Negative value otherwise.
 */
int create_proc_node_info(kerrighed_node_t nodeid)
{
	struct proc_dir_entry *node_procfs;
	char dir_name[80];

	/* Create the ? /proc/nodes/node<x> ? entry */

	if (nodeid == KERRIGHED_MAX_NODES)
		snprintf(dir_name, 80, "cluster");
	else
		snprintf(dir_name, 80, "node%d", nodeid);

	DEBUG(DEBUG_KRG_PROCFS, 2, "Creating /proc/nodes/%s\n", dir_name);

	node_procfs = create_proc_entry(dir_name, S_IFDIR | S_IRUGO | S_IWUGO |
					S_IXUGO, procfs_nodes);

	if (node_procfs == NULL)
		return -ENOMEM;

	/* Create entries in /proc/nodes/node<x> */

	if (nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("nodeid", 0, &proc_krg_nodeid_operations,
				     node_procfs, (void *)((long)nodeid));
	krg_create_seq_entry("session", 0, &proc_krg_session_operations,
			     node_procfs, (void *)((long)nodeid));
#ifdef CONFIG_CLUSTER_WIDE_PROC_CPUINFO
#define CW_CPUINFO 1
#else
#define CW_CPUINFO 0
#endif
	if (CW_CPUINFO || nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("cpuinfo", 0, &proc_krg_cpuinfo_operations,
				     node_procfs, (void *)((long)nodeid));
#ifdef CONFIG_CLUSTER_WIDE_PROC_MEMINFO
#define CW_MEMINFO 1
#else
#define CW_MEMINFO 0
#endif
	if (CW_MEMINFO || nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("meminfo", 0, &proc_krg_meminfo_operations,
				     node_procfs, (void *)((long)nodeid));
#ifdef CONFIG_CLUSTER_WIDE_PROC_LOADAVG
#define CW_LOADAVG 1
#else
#define CW_LOADAVG 0
#endif
	if (CW_LOADAVG || nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("loadavg", 0, &proc_krg_loadavg_operations,
				     node_procfs, (void *)((long)nodeid));
#ifdef CONFIG_KRG_SCHED_MOSIX_PROBE
	krg_create_seq_entry("mosix_load", 0, &proc_krg_mosix_load_operations,
			     node_procfs, (void *)((long)nodeid));
#endif
#ifdef CONFIG_CLUSTER_WIDE_PROC_UPTIME
#define CW_UPTIME 1
#else
#define CW_UPTIME 0
#endif
	if (CW_UPTIME || nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("uptime", 0, &proc_krg_uptime_operations,
				     node_procfs, (void *)((long)nodeid));
#ifdef CONFIG_CLUSTER_WIDE_PROC_STAT
#define CW_STAT 1
#else
#define CW_STAT 0
#endif
	if (CW_STAT || nodeid != KERRIGHED_MAX_NODES)
		krg_create_seq_entry("stat", 0, &proc_krg_stat_operations,
				     node_procfs, (void *)((long)nodeid));

	if (nodeid == kerrighed_node_id)
		proc_symlink("self", procfs_nodes, dir_name);

	return 0;
}

/** Remove a /proc/nodes/node<x> directory and sub-files.
 *  @author Matthieu Fertré
 *
 *  @param nodeid   Id of the node to remove a proc entry for.
 *
 *  @return  0 if everything ok.
 *           Negative value otherwise.
 */
int remove_proc_node_info(kerrighed_node_t nodeid)
{
	struct proc_dir_entry *subdir, *next;
	char dir_name[80];
	
	snprintf(dir_name, 80, "node%d", nodeid);
	DEBUG(DEBUG_KRG_PROCFS, 2, "Removing /proc/nodes/%s\n", dir_name);

	subdir = procfs_nodes->subdir;
	if (subdir) {
		for (next = subdir->next; next;
                     subdir = next, next = subdir->next)
			if (!strcmp(subdir->name, dir_name)){
				procfs_deltree(subdir);
				return 0;
			}
	}

	return -ENOENT;
}

/** Init Kerrighed proc stuffs.
 *  @author Renaud Lottiaux
 */
int krg_procfs_init()
{
	DEBUG(DEBUG_KRG_PROCFS, 1, "krg_procfs_init\n");

	/* Create the ? /proc/kerrighed/nodes ? entry */

#ifdef CONFIG_CLUSTER_WIDE_PROC
	procfs_nodes = create_proc_entry("nodes", S_IFDIR | S_IRUGO | S_IWUGO |
					 S_IXUGO, NULL);
#else
	procfs_nodes = create_proc_entry("nodes", S_IFDIR | S_IRUGO | S_IWUGO |
					 S_IXUGO, proc_kerrighed);
#endif

	if (procfs_nodes == NULL) {
		WARNING("Cannot create /proc/kerrighed/nodes\n");
		return -ENOMEM;
	}

	/* Create the ? /proc/kerrighed/nodes/nrnodes ? entry */

	procfs_nrnodes =
	    create_proc_read_entry("nrnodes", S_IRUGO, procfs_nodes,
				   krg_nrnodes_read_proc, NULL);

	if (procfs_nrnodes == NULL) {
		WARNING("Cannot create /proc/kerrighed/nodes/nrnodes\n");
		return -ENOMEM;
	}

	/* Create cluster-wide entries in /proc/ */

#ifdef CONFIG_CLUSTER_WIDE_PROC_CPUINFO
	remove_proc_entry("cpuinfo", NULL);
	krg_create_seq_entry("cpuinfo", 0, &proc_krg_cpuinfo_operations,
			     NULL,
			     (void *)((int)PROC_STAT_DEPEND_ON_CAPABILITY));
#endif
#ifdef CONFIG_CLUSTER_WIDE_PROC_MEMINFO
	remove_proc_entry("meminfo", NULL);
	krg_create_seq_entry("meminfo", 0, &proc_krg_meminfo_operations, NULL,
			     (void *)((int)PROC_STAT_DEPEND_ON_CAPABILITY));
#endif
#ifdef CONFIG_CLUSTER_WIDE_PROC_LOADAVG
	remove_proc_entry("loadavg", NULL);
	krg_create_seq_entry("loadavg", 0, &proc_krg_loadavg_operations, NULL,
			     (void *)((int)PROC_STAT_DEPEND_ON_CAPABILITY));
#endif
#ifdef CONFIG_CLUSTER_WIDE_PROC_STAT
	remove_proc_entry("stat", NULL);
	krg_create_seq_entry("stat", 0, &proc_krg_stat_operations, NULL,
			     (void *)((int)PROC_STAT_DEPEND_ON_CAPABILITY));
#endif
#ifdef CONFIG_CLUSTER_WIDE_PROC_UPTIME
	remove_proc_entry("uptime", NULL);
	krg_create_seq_entry("uptime", 0, &proc_krg_uptime_operations, NULL,
			     (void *)((int)PROC_STAT_DEPEND_ON_CAPABILITY));
#endif
	//  proc_net_remove("dev");

#ifdef CONFIG_CLUSTER_WIDE_PROC_INFRA
	/* Create the ? /proc/kerrighed/nodes/cluster ? entry */

	create_proc_node_info(KERRIGHED_MAX_NODES);
#endif

	DEBUG(DEBUG_KRG_PROCFS, 1, "krg_procfs_init : done\n");

	return 0;
}

/** Finalize Kerrighed proc stuffs.
 *  @author Renaud Lottiaux
 */
int krg_procfs_finalize()
{
	procfs_deltree(procfs_nodes);

	return 0;
}
