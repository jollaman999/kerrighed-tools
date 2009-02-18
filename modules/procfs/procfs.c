/** Initialization of procfs stuffs for ProcFS module.
 *  @file procfs.c
 *
 *  Copyright (C) 2001-2006, INRIA, Universite de Rennes 1, EDF.
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 */

#define MODULE_NAME "Proc FS Module "

#include "debug_procfs.h"

#include "proc.h"
#include "proc_pid_info.h"
#include "static_node_info_linker.h"
#include "static_cpu_info_linker.h"
#include "dynamic_node_info_linker.h"
#include "dynamic_cpu_info_linker.h"

int procfs_hotplug_init(void);
void procfs_hotplug_cleanup(void);

int init_procfs(void)
{
	/* Create the static node info container */

	init_static_node_info_ctnr();
	init_static_cpu_info_ctnr();
	init_dynamic_node_info_ctnr();
	init_dynamic_cpu_info_ctnr();

#ifdef CONFIG_KRG_PROC
	proc_pid_info_init();
#endif

	krg_procfs_init();

	procfs_hotplug_init();

	return 0;
};

void cleanup_procfs(void)
{
	procfs_hotplug_cleanup();
	krg_procfs_finalize();

#ifdef CONFIG_KRG_PROC
	proc_pid_info_finalize();
#endif
};
