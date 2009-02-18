/** Kerfs module initialization.
 *  @file module.c
 *
 *  Copyright (C) 2001-2006, INRIA, Universite de Rennes 1, EDF.
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 *
 *  Implementation of functions used to initialize and finalize the
 *  kerfs module.
 */

#include "debug_fs.h"

#include <linux/module.h>
#include <linux/proc_fs.h>

#include <ctnr/procfs.h>
#include <ctnr/kddm.h>

#include "regular_file_mgr.h"
#include "file_struct_io_linker.h"
#include "file.h"
#include "mobility.h"
#ifdef CONFIG_KRG_FAF
#include "faf/faf.h"
#endif


/** Initialisation of the DVFS module.
 *  @author Renaud Lottiaux
 *
 *  Start DVFS server.
 */
int init_dvfs (void)
{
	printk ("DVFS initialisation : start\n");

	dvfs_file_cachep = kmem_cache_create("dvfs_file",
					     sizeof(struct dvfs_file_struct),
					     0, SLAB_PANIC, NULL, NULL);
	

#ifdef CONFIG_KRG_DEBUG
	init_dvfs_debug();
#endif
	register_io_linker (DVFS_FILE_STRUCT_LINKER,
			    &dvfs_file_struct_io_linker);
	
#ifdef CONFIG_KRG_EPM
	dvfs_mobility_init();
#endif
#ifdef CONFIG_KRG_FAF
	faf_init();
#endif
	dvfs_file_init();
		
	printk ("DVFS initialisation done\n");
	
	return 0;
}



/** Cleanup of the DVFS module.
 *  @author Renaud Lottiaux
 *
 *  Kill DVFS server.
 */
void cleanup_dvfs (void)
{
	printk ("DVFS termination : start\n");
	
#ifdef CONFIG_KRG_FAF
	faf_finalize() ;
#endif
	dvfs_file_finalize();
#ifdef CONFIG_KRG_EPM
	dvfs_mobility_finalize();
#endif
	
	printk ("DVFS termination done\n");
}
