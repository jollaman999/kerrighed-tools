/*
 *  Kerrighed/modules/epm/app_restart.c
 *
 *  Copyright (C) 1999-2006 INRIA, Universite de Rennes 1, EDF
 *  Copyright (C) 2007-2008 Matthieu Fertré - INRIA
 */

#include <linux/compile.h>
#include <linux/version.h>

/* -------------------------- DEBUG */
#include <epm/debug_epm.h>

#define MODULE_NAME "Application Restart"

#include <tools/process.h>
#include <rpc/rpcid.h>
#include <rpc/rpc.h>
#include <ghost/ghost.h>
#include <proc/pid.h>
#include <proc/pid_mobility.h>
#include <proc/task.h>
#include <tools/syscalls.h>
#include <epm/restart.h>
#include <kerrighed/pid.h>
#include <ctnr/kddm.h>
#include <epm/application/application.h>
#include <epm/application/app_shared.h>
#include <epm/application/app_utils.h>

#include <kerrighed/task.h>
#include <kerrighed/children.h>
#include <proc/children.h>

extern struct task_struct *baby_sitter;

static inline int restore_app_kddm_object(struct app_kddm_object *obj,
					  long app_id, int chkpt_sn,
					  const credentials_t *user_creds)
{
	ghost_fs_t oldfs;
	ghost_t *ghost;
	long tmpl;
	int tmpi;
	int r = 0;
	int magic = 4342338;
	u32 linux_version;
	char compile_info[MAX_GHOST_STRING];

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app_id);

	oldfs = set_ghost_fs(user_creds->uid, user_creds->gid);

	ghost = create_file_ghost(GHOST_READ, app_id, chkpt_sn,
				  -1, "global");

	if (IS_ERR(ghost)) {
		r = PTR_ERR(ghost);
		goto err_open;
	}

	/* check some information about the Linux kernel version */
	r = ghost_read(ghost, &linux_version, sizeof(linux_version));
	if (r)
		goto err_read;
	if (linux_version != LINUX_VERSION_CODE)
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(UTS_MACHINE, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(UTS_VERSION, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(LINUX_COMPILE_TIME, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(LINUX_COMPILE_BY, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(LINUX_COMPILE_HOST, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	r = ghost_read_string(ghost, compile_info);
	if (r)
		goto err_read;
	if (strncmp(LINUX_COMPILER, compile_info, MAX_GHOST_STRING))
		goto err_kernel_version;

	/* check some information about the checkpoint itself */
	r = ghost_read(ghost, &tmpl, sizeof(tmpl));
	if (r)
		goto err_read;

	if (tmpl != app_id) {
		r = -E_CR_BADDATA;
		goto err_read;
	}

	r = ghost_read(ghost, &tmpi, sizeof(tmpi));
	if (r)
		goto err_read;

	if (tmpi != chkpt_sn) {
		r = -E_CR_BADDATA;
		goto err_read;
	}

	/* initialize app_kddm_object */
	obj->app_id = app_id;
	obj->chkpt_sn = chkpt_sn;

	r = ghost_read(ghost, &obj->nodes, sizeof(obj->nodes));
	if (r)
		goto err_read;

	r = ghost_read(ghost, &tmpi, sizeof(tmpi));
	if (r)
		goto err_read;

	if (tmpi != magic) {
		r = -E_CR_BADDATA;
		goto err_read;
	}

err_read:
	/* End of the really interesting part */
	ghost_close(ghost);

err_open:
	unset_ghost_fs(&oldfs);

	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, r=%d\n", app_id, r);
	return r;

err_kernel_version:
	printk("Try to restart a checkpoint written from "
		       "another kernel: aborting.\n");
	r = -E_CR_BADDATA;
	goto err_read;
}

static inline int read_task_parent_links(struct app_struct *app,
					 ghost_t *ghost,
					 pid_t pid)
{
	int r = 0;
	task_state_t *task_desc = NULL;
	pid_t tgid, parent, real_parent, real_parent_tgid;
	pid_t pgrp, session;

	r = ghost_read(ghost, &tgid, sizeof(pid_t));
	if (r)
		goto err_read;
	r = ghost_read(ghost, &parent, sizeof(pid_t));
	if (r)
		goto err_read;
	r = ghost_read(ghost, &real_parent, sizeof(pid_t));
	if (r)
		goto err_read;
	r = ghost_read(ghost, &real_parent_tgid, sizeof(pid_t));
	if (r)
		goto err_read;
	r = ghost_read(ghost, &pgrp, sizeof(pid_t));
	if (r)
		goto err_read;
	r = ghost_read(ghost, &session, sizeof(pid_t));
	if (r)
		goto err_read;

	task_desc = alloc_task_state_from_pids(pid, tgid,
					       parent,
					       real_parent,
					       real_parent_tgid,
					       pgrp, session);

	DEBUG(DBG_APP_CKPT, 2, "Import Process [%d], pgrp:%d,"
	      "Parent (real/tgid): %d (%d/%d)\n",
	      pid, pgrp, parent, real_parent, real_parent_tgid);

	if (IS_ERR(task_desc))
		goto err_alloc;

	spin_lock(&app->lock);
	list_add_tail(&task_desc->next_task, &app->tasks);
	spin_unlock(&app->lock);

err_read:
err_alloc:
	return r;
}

static inline int restore_local_app(long app_id, int chkpt_sn,
				    kerrighed_node_t node_id,
				    int duplicate,
				    const credentials_t *user_creds)
{
	int r = 0;
	ghost_fs_t oldfs;
	ghost_t *ghost;
	pid_t pid;
	pid_t null = -1;
	pid_t prev = -1;

	struct app_struct *app = NULL;
	kerrighed_node_t r_node_id;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld, nodeid: %d, duplicate: %d\n",
	      app_id, node_id, duplicate);

	oldfs = set_ghost_fs(user_creds->uid, user_creds->gid);

	ghost = create_file_ghost(GHOST_READ, app_id, chkpt_sn,
				  node_id, "node");

	if (IS_ERR(ghost)) {
		r = PTR_ERR(ghost);
		goto err_open;
	}

	if (node_id == kerrighed_node_id || !duplicate) {
		app = new_local_app(app_id);
	} else {
		do {
			DEBUG(DBG_APP_CKPT, 2, "%d waiting creation of initial "
			      " app_struct (replaces node %d)\n",
			      current->pid, node_id);
			sleep(1);
			DEBUG(DBG_APP_CKPT, 2, "End of waiting creation of"
			      " initial app_struct\n");
			app = find_local_app(app_id);
		} while (app == NULL);
	}
	BUG_ON(!app);

	app->chkpt_sn = chkpt_sn;

	/* read the node_id */
	r = ghost_read(ghost, &r_node_id, sizeof(kerrighed_node_t));
	if (r)
		goto err_read;

	if (r_node_id != node_id) {
		r = -E_CR_BADDATA;
		goto err_read;
	}

	/* get description of each process */
	r = ghost_read(ghost, &pid, sizeof(pid_t));
	if (r)
		goto err_read;

	if (pid == null) {
		/* there must be at least one process */
		r = -E_CR_BADDATA;
		goto err_read;
	}

	while (pid != null) {

		r = read_task_parent_links(app, ghost, pid);
		if (r)
			goto err_read;

		/* next! */
		prev = pid;
		r = ghost_read(ghost, &pid, sizeof(pid_t));
		if (r)
			goto err_read;

		/* a process must to not be twice in the checkpoint! */
		BUG_ON(pid == prev);
	}

err_read:
	/* End of the really interesting part */
	ghost_close(ghost);

err_open:
	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, r=%d\n", app_id, r);

	unset_ghost_fs(&oldfs);

	/* the local app_struct will be deleted later in case of error */
	return r;
}


/*--------------------------------------------------------------------------*/

static inline int __local_init_restart(long app_id, int chkpt_sn,
				       kerrighed_node_t node_id, int duplicate,
				       const credentials_t *user_creds)
{
	int r;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app_id);

	r = restore_local_app(app_id, chkpt_sn, node_id,
			      duplicate, user_creds);
	return r;
}

struct init_restart_msg {
	kerrighed_node_t requester;
	long app_id;
	int chkpt_sn;
	int recovery;
	credentials_t user_creds;
};

static void handle_init_restart(struct rpc_desc *desc, void *_msg, size_t size)
{
	struct init_restart_msg *msg = _msg;
	kerrighed_node_t n = kerrighed_node_id;
	int duplicate = 0;
	int r;

	DEBUG(DBG_APP_CKPT, 1, "app_id : %ld\n", msg->app_id);

	if (msg->recovery) {
		r = rpc_unpack_type(desc, n);
		if (r)
			goto err_rpc;
		r = rpc_unpack_type(desc, duplicate);
		if (r)
			goto err_rpc;
	}

	r = __local_init_restart(msg->app_id, msg->chkpt_sn, n, duplicate,
				 &msg->user_creds);
	r = rpc_pack_type(desc, r);
	if (r)
		goto err_rpc;

	return;

err_rpc:
	rpc_cancel(desc);
	return;
}


static inline kerrighed_node_t
__find_node_for_restart(kerrighed_node_t *first_avail_node,
			int *duplicate,
			struct app_kddm_object *obj,
			kerrighed_node_t node_id)
{
	int n;

	/* looking for a node not involved in the application */
	for (n = krgnode_next_possible(*first_avail_node) ;
	     n < KERRIGHED_MAX_NODES;
	     n = krgnode_next_possible(n)) {

		if (!krgnode_isset(n, obj->nodes)) {
			krgnode_set(n, obj->nodes);
			goto out;
		}
	}

	/* all nodes are implied in the application,
	   selecting the first existing node... */
	for (n = krgnode_next_possible(0); n < KERRIGHED_MAX_NODES;
	     n = krgnode_next_possible(n)) {
		*first_avail_node = n+1;
		*duplicate = 1;
		goto out;
	}

	BUG();

out:
	*first_avail_node = n+1;
	DEBUG(DBG_APP_CKPT, 5, "EMERGENCY- Node %d have been chosen to replace node %d\n",
	      n, node_id);
	return n;
}


static int global_init_restart(struct app_kddm_object *obj, int chkpt_sn,
			       const credentials_t *user_creds)
{
	struct rpc_desc *desc;
	struct init_restart_msg msg;
	krgnodemask_t nodes, nodes_to_replace;
	kerrighed_node_t prev_available_node = 0;
	kerrighed_node_t node, recovery_node;
	int duplicate = 0;
	int r;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", obj->app_id);

	r = restore_app_kddm_object(obj, obj->app_id, chkpt_sn, user_creds);
	if (r)
		goto exit;

	/* prepare message */
	msg.requester = kerrighed_node_id;
	msg.app_id = obj->app_id;
	msg.chkpt_sn = chkpt_sn;
	msg.user_creds = *user_creds;
	msg.recovery = 0;

	/* prepare nodes vector */
	krgnodes_clear(nodes);
	krgnodes_clear(nodes_to_replace);
	for_each_krgnode_mask(node, obj->nodes){
		DEBUG(DBG_APP_CKPT, 5, "Adding %d in vector ?\n", node);

		if (likely(krgnode_possible(node)))
			krgnode_set(node, nodes);
		else
			krgnode_set(node, nodes_to_replace);
	}

	if (!krgnodes_empty(nodes)) {
		desc = rpc_begin_m(APP_INIT_RESTART, &nodes);

		r = rpc_pack_type(desc, msg);
		if (r)
			goto err_rpc;

		/* waiting results */
		r = app_wait_returns_from_nodes(desc, nodes);
		rpc_end(desc, 0);
	}

	/* some nodes may be unavailable */
	msg.recovery = 1;
	for_each_krgnode_mask(node, nodes_to_replace) {
		duplicate = 0;

		recovery_node = __find_node_for_restart(
			&prev_available_node, &duplicate, obj, node);

		krgnode_set(recovery_node, nodes);

		desc = rpc_begin(APP_INIT_RESTART, recovery_node);
		r = rpc_pack_type(desc, msg);
		if (r)
			goto err_rpc;

		r = rpc_pack_type(desc, node);
		if (r)
			goto err_rpc;

		r = rpc_pack_type(desc, duplicate);
		if (r)
			goto err_rpc;

		DEBUG(DBG_APP_CKPT, 5, "EMERGENCY- Waiting returns from %d (%d)\n",
		      recovery_node, node);

		r = rpc_unpack_type_from(desc, recovery_node, r);
		if (r)
			goto err_rpc;

		DEBUG(DBG_APP_CKPT, 5, "EMERGENCY- %d (%d) returns %d\n",
		      recovery_node, node, r);

		rpc_end(desc, 0);
	}

	krgnodes_copy(obj->nodes, nodes);
exit:
	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld - r=%d\n", obj->app_id, r);
	return r;

err_rpc:
	rpc_cancel(desc);
	rpc_end(desc, 0);
	goto exit;
}


/*--------------------------------------------------------------------------*/

enum process_role {
	THREAD_LEADER,
	PGRP_LEADER,
	SESSION_LEADER,
	NOT_A_LEADER,
};

static inline int is_thread_leader(task_state_t *t)
{
	return (t->restart.tgid == t->restart.pid);
}

static inline int is_pgrp_leader(task_state_t *t)
{
	return (t->restart.pgrp == t->restart.pid);
}

static inline int is_session_leader(task_state_t *t)
{
	return (t->restart.session == t->restart.pid);
}

static inline int __restart_process(struct app_struct *app, media_t media,
				    const credentials_t *user_creds,
				    task_state_t *t)
{
	int r = 0;
	struct task_struct *task;
	task = restart_process(media, t->restart.pid,
			       app->app_id, app->chkpt_sn,
			       user_creds);
	if (IS_ERR(task)) {
		r = PTR_ERR(task);
		goto error;
	}

	/* Attach to application */
	BUG_ON(!task);
	task->application = app;
	t->task = task;
error:
	return r;
}

static inline int was_checkpointed(struct app_struct *app, pid_t pid)
{
	/* What is the right way to check that ? */

	int error;
	struct nameidata nd;

	char *filename = get_chkpt_filebase(app->app_id, app->chkpt_sn,
					    pid, "task");
	error = path_lookup(filename, 0, &nd);
	if (!error)
		path_release(&nd);
	kfree(filename);

	if (!error)
		return 1;

	return 0;
}

typedef struct {
	int nb;
	struct list_head pids;
} pids_list_t;

typedef struct {
	pid_t pid;
	int reserved;
	struct list_head next;
} unique_pid_t;

static inline int add_unique_pid(pids_list_t *orphan_pids, pid_t pid)
{
	int r = 0;
	unique_pid_t *upid;

	DEBUG(DBG_APP_CKPT, 5, "try to add %d\n", pid);

	/* check the pid is not already in the list */
	list_for_each_entry(upid, &(orphan_pids->pids), next) {
		if (upid->pid == pid)
			goto end;
	}

	DEBUG(DBG_APP_CKPT, 5, "adding %d ...\n", pid);

	/* add the pid in the list */
	upid = kmalloc(sizeof(unique_pid_t), GFP_KERNEL);
	if (!upid) {
		r = -ENOMEM;
		goto end;
	}
	upid->pid = pid;
	upid->reserved = 0;
	list_add_tail(&upid->next, &(orphan_pids->pids));
	orphan_pids->nb++;
end:
	DEBUG(DBG_APP_CKPT, 5, "DONE: %d\n", r);
	return r;
}

static inline int send_pids_list(pids_list_t *orphan_pids,
				  struct rpc_desc *desc)
{
	int r = 0;
	unique_pid_t *upid;

	DEBUG(DBG_APP_CKPT, 5, "Begin\n");

	r = rpc_pack_type(desc, orphan_pids->nb);
	if (r)
		goto err;
	list_for_each_entry(upid, &(orphan_pids->pids), next) {
		r = rpc_pack_type(desc, upid->pid);
		if (r)
			goto err;
	}

err:
	DEBUG(DBG_APP_CKPT, 5, "End - r=%d\n", r);
	return r;
}

static inline void free_pids_list(pids_list_t *orphan_pids)
{
	unique_pid_t *upid;
	struct list_head *element, *tmp;

	DEBUG(DBG_APP_CKPT, 5, "Begin\n");

	list_for_each_safe(element, tmp, &(orphan_pids->pids)) {
		upid = list_entry(element, unique_pid_t, next);

		list_del(element);
		kfree(upid);
	}
	orphan_pids->nb = 0;

	DEBUG(DBG_APP_CKPT, 5, "End\n");
}

static inline int return_orphan_sessions_and_prgps(struct app_struct *app,
						   struct rpc_desc *desc)
{
	int r = 0;
	task_state_t *t;
	pids_list_t orphan_pids;
	INIT_LIST_HEAD(&orphan_pids.pids);
	orphan_pids.nb = 0;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	/* first, build a list of orphan pids of session(s) and pgrp(s) */
	list_for_each_entry(t, &app->tasks, next_task) {
		if (!was_checkpointed(app, t->restart.session)) {
			r = add_unique_pid(&orphan_pids, t->restart.session);
			if (r)
				goto err;
		}

		if (!was_checkpointed(app, t->restart.pgrp)) {
			r = add_unique_pid(&orphan_pids, t->restart.pgrp);
			if (r)
				goto err;
		}
	}

	/* secondly, send it to the global coordinator */
	r = send_pids_list(&orphan_pids, desc);

err:
	/* thirdly, free the list */
	free_pids_list(&orphan_pids);
	return r;
}

static inline int get_orphan_sessions_and_pgrps(struct rpc_desc *desc,
						krgnodemask_t nodes,
						pids_list_t *orphan_pids)
{
	kerrighed_node_t node;
	int i, r=0;

	INIT_LIST_HEAD(&orphan_pids->pids);
	orphan_pids->nb = 0;

	for_each_krgnode_mask(node, nodes) {
		int local_orphans;
		pid_t pid;

		r = rpc_unpack_type_from(desc, node, local_orphans);
		if (r)
			goto err;

		for (i=0; i< local_orphans; i++) {
			r = rpc_unpack_type_from(desc, node, pid);
			if (r)
				goto err;

			r = add_unique_pid(orphan_pids, pid);
			if (r)
				goto err;
		}

		r = rpc_unpack_type_from(desc, node, r);
		if (r)
			goto err;
        }

out:
	return r;
err:
	free_pids_list(orphan_pids);
	goto out;
}

static inline int unrebuild_orphan_pids(pids_list_t *orphan_pids)
{
	int r = 0, ret;
	unique_pid_t *upid;

	list_for_each_entry(upid, &(orphan_pids->pids), next) {
		if (upid->reserved) {
			ret = cancel_pid_reservation(upid->pid);
			if (ret)
				r |= ret;
			else
				upid->reserved = 0;
		}
	}

	return r;
}

static inline int rebuild_orphan_pids(pids_list_t *orphan_pids)
{
	int r = 0;
	unique_pid_t *upid;

	list_for_each_entry(upid, &(orphan_pids->pids), next) {
		BUG_ON(upid->reserved);
		r = reserve_pid(upid->pid);
		if (r)
			goto err;
		upid->reserved = 1;
	}

err:
	return r;
}

static inline int local_do_restart(struct app_struct *app, media_t media,
				   const credentials_t *user_creds,
				   enum process_role role)
{
	int r = 0;
	task_state_t *t;

	BUG_ON(app == NULL);
	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);
	BUG_ON(list_empty(&app->tasks));

	list_for_each_entry(t, &app->tasks, next_task) {

		if ((role == SESSION_LEADER && is_session_leader(t))
		    || (role == PGRP_LEADER && is_pgrp_leader(t)
			&& !t->task)
		    || (role == THREAD_LEADER && is_thread_leader(t)
			&& !t->task)
		    || (role == NOT_A_LEADER && !t->task)) {

			BUG_ON(t->task);

			r = __restart_process(app, media, user_creds, t);
			if (r)
				goto exit;
		}
	}
exit:
	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n", app->app_id, r);
	return r;
}

static void local_replace_parent(struct app_struct *app,
				 const task_identity_t *requester)
{
	task_state_t *t;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	list_for_each_entry(t, &app->tasks, next_task) {

		if (!was_checkpointed(app, t->restart.parent)) {
			if (t->restart.parent != 1) {
				/* parent was not checkpointed and was not
				   "init" process, we will reparent to the
				   restart cmd */
				t->restart.parent = requester->pid;
				t->restart.real_parent = requester->pid;
				t->restart.real_parent_tgid = requester->tgid;
			}
		}
	}
}

static int local_restore_task_object(struct app_struct *app)
{
	int r = 0;
	task_state_t *t;
	struct task_struct *task;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	list_for_each_entry(t, &app->tasks, next_task) {
		DEBUG(DBG_APP_CKPT, 3, "pid:%d, tgid: %d, parent:%d, pgrp:%d\n",
		      t->restart.pid, t->restart.tgid, t->restart.parent,
		      t->restart.pgrp);
		task = t->task;

		if (t->restart.parent != 1) {

			task->task_obj = kcb_task_writelock(task->pid);

			DEBUG(DBG_APP_CKPT, 3,
			      "pid:%d, tgid: %d, parent:%d, real_parent:%d\n",
			      t->restart.pid, t->restart.tgid,
			      t->restart.parent, t->restart.real_parent);

			task->task_obj->parent = t->restart.parent;
			task->task_obj->real_parent = t->restart.real_parent;
			task->task_obj->real_parent_tgid =
				t->restart.real_parent_tgid;

			task->parent = baby_sitter;
			task->real_parent = baby_sitter;
			task->group_leader = baby_sitter;

			kcb_task_unlock(task->pid);
		}

		if (t->restart.pid != t->restart.tgid) {

			if (!was_checkpointed(app, t->restart.tgid)) {
				r = -E_CR_BADDATA;
				goto error;
			}
			task->task_obj = kcb_task_writelock(task->pid);

			task->task_obj->group_leader = t->restart.tgid;

			task->parent = baby_sitter;
			task->real_parent = baby_sitter;
			task->group_leader = baby_sitter;

			kcb_task_unlock(task->pid);
		}
	}

error:
	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n", app->app_id, r);
	return r;
}

static inline int task_restore_children_object(task_state_t *t)
{
	int r = 0;
	struct children_kddm_object *obj;

	if (t->restart.real_parent_tgid == 1)
		goto exit;

	obj = kh_children_writelock(t->restart.real_parent_tgid);

	DEBUG(DBG_APP_CKPT, 3, "pid:%d, tgid:%d/%d, parent:%d, parent_tgid:%d\n",
	      t->restart.pid, t->restart.tgid, t->task->task_obj->group_leader,
	      t->restart.real_parent, t->restart.real_parent_tgid);

	r = kh_new_child(obj,
			 t->restart.real_parent,
			 t->restart.pid,
			 t->restart.tgid,
			 process_group(t->task),
			 process_session(t->task),
			 t->task->exit_signal);

	t->task->parent_children_obj = obj;
	kh_children_get(t->task->parent_children_obj);

	kh_children_unlock(t->restart.real_parent_tgid);

exit:
	return r;
}

static inline int local_restore_children_object(struct app_struct *app)
{
	int r = 0;
	task_state_t *t;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	list_for_each_entry(t, &app->tasks, next_task) {
		task_restore_children_object(t);
	}

	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n", app->app_id, r);
	return r;
}

static inline struct task_struct *
find_thread_leader(struct app_struct *app, task_state_t *th)
{
	task_state_t *t;
	struct task_struct *leader = NULL;

	list_for_each_entry(t, &app->tasks, next_task) {
		if (t->restart.pid == th->restart.tgid) {
			BUG_ON(t->restart.pid != t->restart.tgid);
			BUG_ON(!t->task);
			leader = t->task;
			goto found;
		}
	}
found:
	return leader;
}

static inline int local_join_relatives(struct app_struct *app)
{
	int r = 0;
	task_state_t *t;
	struct task_struct *tsk;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	list_for_each_entry(t, &app->tasks, next_task) {
		tsk = t->task;

		DEBUG(DBG_APP_CKPT, 5, "Task: %d (tgid: %d/%d)\n"
		      "parent: %d\n"
		      "real parent: %d (tgid: %d)\n",
		      tsk->pid, tsk->tgid,
		      tsk->task_obj->group_leader,
		      tsk->task_obj->parent,
		      tsk->task_obj->real_parent,
		      tsk->task_obj->real_parent_tgid);

		join_local_relatives(tsk);
		pid_link_task(tsk->pid);

		DEBUG(DBG_APP_CKPT, 5, "Task: %d (tgid: %d/%d)\n"
		      "parent: %d\n"
		      "real parent: %d (tgid: %d)\n",
		      tsk->pid, tsk->tgid,
		      tsk->task_obj->group_leader,
		      tsk->task_obj->parent,
		      tsk->task_obj->real_parent,
		      tsk->task_obj->real_parent_tgid);

		if (!is_thread_leader(t)) {
			struct task_struct *leader = find_thread_leader(app, t);
			if (!leader) {
				r = -E_CR_BADDATA;
				goto err;
			}

			BUG_ON(tsk->tgid != leader->tgid);

			krg_rehash_restarted_thread(tsk, leader);
		}
	}

	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n", app->app_id, r);
err:
	return r;
}

/* After local_abort_restart, the local app_struct does not exit
   anymore and the global application may not exist (it depends
   of other nodes) */
static void local_abort_restart(struct app_struct *app,
				struct task_struct *fake)
{
	struct list_head *element;
	task_state_t *t;
	struct task_struct *task;
	int r;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", app->app_id);

	/* killall restarted processes */
	spin_lock(&app->lock);
	while (!list_empty(&app->tasks)) {
		element = (&app->tasks)->next;
		t = list_entry(element, task_state_t, next_task);
		task = t->task;
		list_del(element);
		free_task_state(t);

		if (task) {
			spin_unlock(&app->lock);

			/* kill the process which was already restarted */
			DEBUG(DBG_APP_CKPT, 1, "Killing %d\n", task->pid);
			/* We first need to unregister the task from the
			   application else the task will try to do it by
			   itself */
			task->application = NULL;
			release_task(task);

			spin_lock(&app->lock);
		}
	}
	spin_unlock(&app->lock);

	/* destroying the shared objects that were restored for nothing */
	if (fake)
		destroy_shared_objects(app, fake);

	r = __delete_local_app(app);
	BUG_ON(r);
	/* The local application does not exist anymore */
}

struct restart_request_msg {
	kerrighed_node_t requester;
	long app_id;
	media_t media;
	credentials_t user_creds;
	task_identity_t requester_task;
};

static void handle_do_restart(struct rpc_desc *desc, void *_msg, size_t size)
{
	int r;
	struct restart_request_msg *msg = _msg;
	struct app_struct *app = find_local_app(msg->app_id);
	struct task_struct *fake = NULL;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", msg->app_id);
	BUG_ON(app == NULL);

	/* return the list of orphan sessions and pgrp */
	r = return_orphan_sessions_and_prgps(app, desc);

	r = send_result(desc, r);
	if (r)
		goto error;

	/* restore the shared objects */
	fake = alloc_task_struct();
	if (!fake) {
		r = -ENOMEM;
		goto error;
	}

	r = local_restart_shared(desc, app, fake, app->chkpt_sn, &msg->user_creds);
	if (r)
		goto error;

	/* restore the session leader(s) */
	r = local_do_restart(app, msg->media, &msg->user_creds,
			     SESSION_LEADER);

	r = send_result(desc, r);
	if (r)
		goto error;

	/* restore the group leader(s) */
	r = local_do_restart(app, msg->media, &msg->user_creds,
			     PGRP_LEADER);

	r = send_result(desc, r);
	if (r)
		goto error;

	/* restore the thread leader(s) */
	r = local_do_restart(app, msg->media, &msg->user_creds,
			     THREAD_LEADER);

	r = send_result(desc, r);
	if (r)
		goto error;

	/* restore the other process(es) */
	r = local_do_restart(app, msg->media, &msg->user_creds,
			     NOT_A_LEADER);

	r = send_result(desc, r);
	if (r)
		goto error;

	local_replace_parent(app, &msg->requester_task);

	/* restore task object */
	r = local_restore_task_object(app);

	r = send_result(desc, r);
	if (r)
		goto error;

	/* restore children object */
	r = local_restore_children_object(app);

	r = send_result(desc, r);
	if (r) /* an error as occured on other node */
		goto error;

	/* join all together */
	r = local_join_relatives(app);

	r = send_result(desc, r);
	if (r)
		goto error;

 	/* complete the import of shared objects */
 	local_restart_shared_complete(app, fake);

error:
	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n", app->app_id, r);

	if (r) {
		local_abort_restart(app, fake);
		app = NULL;
		r = rpc_pack_type(desc, r);
		if (r)
			rpc_cancel(desc);
	}

	if (fake)
		free_task_struct(fake);
}

static int global_do_restart(struct app_kddm_object *obj, media_t media,
			     const credentials_t *user_creds,
			     const task_identity_t *requester)
{
	struct rpc_desc *desc;
	struct restart_request_msg msg;
	pids_list_t orphan_pids;
	int r=0;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld\n", obj->app_id);

	/* prepare message */
	msg.requester = kerrighed_node_id;
	msg.app_id = obj->app_id;
	msg.media = media;
	msg.user_creds = *user_creds;
	msg.requester_task = *requester;

	desc = rpc_begin_m(APP_DO_RESTART, &obj->nodes);
	if (!desc)
		return -ENOMEM;

	r = rpc_pack_type(desc, msg);
	if (r)
		goto err_no_pids;

	/* get the list of orphan sessions/groups */
	r = get_orphan_sessions_and_pgrps(desc, obj->nodes, &orphan_pids);
	if (r)
		goto err_no_pids;

	/* reserve orphan session/pgrp pids */
	r = rebuild_orphan_pids(&orphan_pids);
	if (r)
		goto error;

	/* loading of shared objects */
	r = rpc_pack_type(desc, r);
	if (r)
		goto error;

 	r = global_restart_shared(desc, obj);
 	if (r)
 		goto error;

	/* asking to rebuild session leader(s) */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* asking to rebuild group leader(s) */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* asking to rebuild thread leader(s) */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* asking to rebuild other processes */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

 	/* asking to rebuild task_kddm_obj if r == 0 */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* asking to rebuild children_object if r == 0 */
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* join all together if r == 0*/
	r = ask_nodes_to_continue(desc, obj->nodes, r);
	if (r)
		goto error;

	/* inform other nodes about current restart status */
	r = rpc_pack_type(desc, r);
	if (r)
		goto error;

exit_free_pid:
	free_pids_list(&orphan_pids);

exit:
	rpc_end(desc, 0);

	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return=%d\n",
	      obj->app_id, r);
	return r;

error:
	unrebuild_orphan_pids(&orphan_pids);
	rpc_cancel(desc);
	goto exit_free_pid;

err_no_pids:
	rpc_cancel(desc);
	goto exit;
}


/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/

/** Main application restarting interface.
 *  @author Matthieu Fertre
 *
 */
int app_restart(restart_request_t *req, credentials_t *user_creds,
		const task_identity_t *requester)
{
	struct app_kddm_object *obj;
	int r = 0;

	DEBUG(DBG_APP_CKPT, 1, "Begin - Appid: %ld v%d\n",
	      req->app_id, req->chkpt_sn);

	obj = kddm_grab_object(kddm_def_ns, APP_KDDM_ID, req->app_id);

	if (obj->app_id == req->app_id) {
		r = -E_CR_APPBUSY;
		goto exit_app_busy;
	}
	obj->app_id = req->app_id;

	/* open the files and recreate the struct app_struct */
	r = global_init_restart(obj, req->chkpt_sn, user_creds);
	if (r)
		goto exit;

	/* recreate all the tasks */
	r = global_do_restart(obj, req->media, user_creds, requester);
	if (r)
		goto exit;

	/* go back to computation */
	global_continue(obj, 1, NULL);

exit:
	if (r)
		kddm_remove_frozen_object(kddm_def_ns, APP_KDDM_ID, req->app_id);
	else
exit_app_busy:
		kddm_put_object(kddm_def_ns, APP_KDDM_ID, req->app_id);

	DEBUG(DBG_APP_CKPT, 1, "End - Appid: %ld, return: %d\n",
	      req->app_id, r);
	return r;
}

void application_restart_rpc_init(void)
{
	rpc_register_void(APP_INIT_RESTART, handle_init_restart, 0);
	rpc_register_void(APP_DO_RESTART, handle_do_restart, 0);
}
