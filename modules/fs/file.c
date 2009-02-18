/** DVFS Level 3 - File struct sharing management.
 *  @file file.c
 *
 *  Copyright (C) 2006-2007, Renaud Lottiaux, Kerlabs.
 */

#include "debug_fs.h"

#include <linux/file.h>
#include <kerrighed/unique_id.h>
#include <kerrighed/dvfs.h>

#ifdef CONFIG_KRG_IPC
#include <ipc/shm_memory_linker.h>
#endif
#ifdef CONFIG_KRG_FAF
#include "faf/faf_hooks.h"
#include "faf/faf.h"
#include "faf/faf_file_mgr.h"
#endif

#include <ctnr/kddm.h>
#include <hotplug/hotplug.h>
#include <epm/action.h>
#include "file.h"
#include "file_struct_io_linker.h"

/* Unique DVFS file struct id generator root */
unique_id_root_t file_struct_unique_id_root;

/* DVFS file struct container */
struct kddm_set *dvfs_file_struct_ctnr = NULL;

int create_ctnr_file_object(struct file *file)
{
	struct dvfs_file_struct *dvfs_file;
	unsigned long file_id;

	file_id = get_unique_id (&file_struct_unique_id_root);

	DEBUG("file", 3, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file_id, NULL, file, DVFS_LOG_ENTER, 0);

	dvfs_file = grab_dvfs_file_struct(file_id);
	BUG_ON (dvfs_file->file != NULL);

	dvfs_file->f_pos = file->f_pos;
	dvfs_file->count = 1;
	dvfs_file->file = NULL;

	/* Make sure we don't put the same file struct in 2 different objects.
	 * The first writing in file->f_objid wins.
	 * The second one is destroyed. We assume this is really unlikely.
	 */
	if (cmpxchg (&file->f_objid, 0, file_id) != 0)
		_kddm_remove_frozen_object(dvfs_file_struct_ctnr, file_id);
	else {
		dvfs_file->file = file;
		put_dvfs_file_struct (file_id);
	}

	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file_id, NULL, file, DVFS_LOG_EXIT_CREATE_OBJID, file_id);

	return 0;
}



/** Check if we need to share a file struct cluster wide and do whatever needed
 *  @author Renaud Lottiaux
 *
 *  @param file    Struct of the file to check the sharing.
 */
void check_file_struct_sharing (int index, struct file *file,
				struct epm_action *action)
{
	DEBUG("file", 4, index, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_ENTER, 0);

	/* Do not share the file struct for FAF files or already shared files*/
	if (file->f_flags & (O_FAF_CLT | O_FAF_SRV | O_KRG_SHARED))
 		goto done;

#ifdef CONFIG_KRG_IPC
	/* Do not share the file struct for Kerrighed SHM files */
	if (file->f_op == &krg_shmem_file_operations)
		goto done;
#endif

	switch (action->type) {
	  case EPM_CHECKPOINT:
		  goto done;

	  case EPM_REMOTE_CLONE:
		  goto share;

	  case EPM_MIGRATE:
		  if (file_count(file) == 1)
			  goto done;
		  break;

	  default:
		  BUG();
	}

share:
	file->f_flags |= O_KRG_SHARED;

done:
	DEBUG("file", 4, index, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_EXIT, 0);
}



void get_dvfs_file(int index, unsigned long objid)
{
	struct dvfs_file_struct *dvfs_file;
	struct file *file;

	dvfs_file = grab_dvfs_file_struct(objid);
	file = dvfs_file->file;

	DEBUG("file", 3, index, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_ENTER, NULL);

	dvfs_file->count++;

	put_dvfs_file_struct (objid);

	DEBUG("file", 2, index, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_EXIT_GET_DVFS, dvfs_file);
}



void put_dvfs_file(int index, struct file *file)
{
	struct dvfs_file_struct *dvfs_file;
	unsigned long objid = file->f_objid;

	dvfs_file = grab_dvfs_file_struct(objid);
	dvfs_file->count--;

#ifdef CONFIG_KRG_FAF
	check_last_faf_client_close(file, dvfs_file);
#endif

	DEBUG("file", 2, index, faf_srv_id(file), faf_srv_fd(file), 
	      objid, NULL, file, DVFS_LOG_ENTER_PUT_DVFS, dvfs_file);

	/* else someone has allocated a new structure during the grab */

	if (dvfs_file->count == 0)
		_kddm_remove_frozen_object (dvfs_file_struct_ctnr, objid);
	else
		put_dvfs_file_struct (objid);

	DEBUG("file", 3, index, -1, -1, objid, NULL, NULL,
	      DVFS_LOG_EXIT, 0);
}



/*****************************************************************************/
/*                                                                           */
/*                                KERNEL HOOKS                               */
/*                                                                           */
/*****************************************************************************/



/** Callback to get fresh position value for the given file struct.
 *  @author Renaud Lottiaux
 *
 *  @param file    Struct of the file to get the position value.
 */
loff_t kcb_file_pos_read(struct file *file)
{
	struct dvfs_file_struct *dvfs_file;
	loff_t pos;

	dvfs_file = get_dvfs_file_struct (file->f_objid);
	
	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_ENTER, dvfs_file);

	pos = dvfs_file->f_pos;
	
	put_dvfs_file_struct (file->f_objid);

	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_EXIT, 0);

	return pos;
}



/** Callback to write the new file position in the file container.
 *  @author Renaud Lottiaux
 *
 *  @param file    Struct of the file to write position value.
 */
void kcb_file_pos_write(struct file *file, loff_t pos)
{
	struct dvfs_file_struct *dvfs_file;

	dvfs_file = grab_dvfs_file_struct (file->f_objid);

	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_ENTER, dvfs_file);

	dvfs_file->f_pos = pos;

	put_dvfs_file_struct (file->f_objid);

	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_EXIT, 0);
}



/** Callback to decrease usage count on a dvfs file struct.
 *  @author Renaud Lottiaux
 *
 *  @param file    Struct of the file to decrease usage counter.
 */
void kcb_put_file(struct file *file)
{
	DEBUG("file", 2, -1, faf_srv_id(file), faf_srv_fd(file), 
	      file->f_objid, NULL, file, DVFS_LOG_ENTER, 0);

	BUG_ON (file->f_objid == 0);

	put_dvfs_file(-1, file);

	DEBUG("file", 3, -1, -1, -1, -1UL, NULL, file, DVFS_LOG_EXIT, 0);
}



/*****************************************************************************/
/*                                                                           */
/*                              INITIALIZATION                               */
/*                                                                           */
/*****************************************************************************/



int dvfs_file_init(void)
{
	init_and_set_unique_id_root (&file_struct_unique_id_root, 1);
	
	/* Create the DVFS file struct container */

	dvfs_file_struct_ctnr = create_new_kddm_set(
		kddm_def_ns,
		DVFS_FILE_STRUCT_KDDM_ID,
		DVFS_FILE_STRUCT_LINKER,
		KDDM_RR_DEF_OWNER,
		sizeof (struct dvfs_file_struct),
		KDDM_LOCAL_EXCLUSIVE);
	
	if (IS_ERR(dvfs_file_struct_ctnr))
		OOM;

	hook_register(&kh_file_pos_read, kcb_file_pos_read);
	hook_register(&kh_file_pos_write, kcb_file_pos_write);
	hook_register(&kh_put_file, kcb_put_file);

	return 0;
}



void dvfs_file_finalize (void)
{
}
