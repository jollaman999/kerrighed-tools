/*
 *  Kerrighed/modules/epm/application.c
 *
 *  Copyright (C) 1999-2006 INRIA, Universite de Rennes 1, EDF
 *  Copyright (C) 2007-2008 Matthieu Fertré - INRIA
 */

#include <linux/sched.h>
#include <kerrighed/kerrighed_signal.h>

#include <epm/debug_epm.h>
#define MODULE_NAME "Application"

#include <rpc/rpcid.h>
#include <rpc/rpc.h>
#include <tools/process.h>
#include <tools/syscalls.h>
#include <ctnr/kddm.h>
#include <epm/action.h>
#include <epm/epm_internal.h>
#include <epm/checkpoint.h>
#include <epm/application/app_checkpoint.h>
#include <epm/application/app_frontier.h>
#include <epm/application/app_restart.h>
#include <epm/application/app_shared.h>
#include <epm/application/app_utils.h>
#include "application.h"

/*--------------------------------------------------------------------------*
 *--------------------------------------------------------------------------*/

static hashtable_t *app_struct_table;


struct app_struct *find_local_app(long app_id)
{
	return (struct app_struct *) hashtable_find(app_struct_table, app_id);
}


/*--------------------------------------------------------------------------*
 *--------------------------------------------------------------------------*/

static struct kddm_set *app_kddm_set;
static struct kmem_cache *app_kddm_obj_cachep;
static struct kmem_cache *app_struct_cachep;
static struct kmem_cache *task_state_cachep;

static int app_alloc_object(struct kddm_obj *obj_entry,
			    struct kddm_set *kddm, objid_t objid)
{
	struct app_kddm_object *a;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid %ld\n", objid);

	a = kmem_cache_alloc(app_kddm_obj_cachep, GFP_KERNEL);
	if (a == NULL)
		return -ENOMEM;

	a->app_id = 0;
	a->chkpt_sn = 0;
	a->state = RUNNING;
	obj_entry->object = a;

	DEBUG(DBG_APPLICATION, 1, "End - Appid %ld\n", objid);
	return 0;
}

static int app_remove_object(void *obj, struct kddm_set *kddm,
			     objid_t objid)
{
	struct app_kddm_object *a = obj;
	kmem_cache_free(app_kddm_obj_cachep, a);

	return 0;
}

static struct iolinker_struct app_io_linker = {
      linker_name:"app ",
      linker_id:APP_LINKER,
      alloc_object:app_alloc_object,
      remove_object:app_remove_object
};


/*--------------------------------------------------------------------------*/

struct app_struct* new_local_app(long app_id)
{
	struct app_struct *app;
	DEBUG(DBG_APPLICATION, 1, "Begin - Appid %ld\n", app_id);

	app = kmem_cache_alloc(app_struct_cachep, GFP_KERNEL);
	app->app_id = app_id;
	app->chkpt_sn = 0;

	spin_lock_init(&app->lock);
	init_completion(&app->tasks_chkpted);
	INIT_LIST_HEAD(&app->tasks);
	app->shared_objects = RB_ROOT;

	hashtable_add(app_struct_table, app_id, app);

	DEBUG(DBG_APPLICATION, 1, "End - Appid %ld\n", app_id);
	return app;
}

int __delete_local_app(struct app_struct *app)
{
	spin_lock(&app->lock);
	/* should be really rare ...*/
	if (!local_tasks_list_empty(app))
		goto exit_wo_deleting;

	DEBUG(DBG_APPLICATION, 1, "Removing local reference to app %ld\n",
	      app->app_id);

	hashtable_remove(app_struct_table, app->app_id);
	spin_unlock(&app->lock);

	clear_shared_objects(app);
	kmem_cache_free(app_struct_cachep, app);
	return 0;

exit_wo_deleting:
	spin_unlock(&app->lock);
	return -EAGAIN;
}

void delete_app(struct app_struct *app)
{
	int r = 0;
	struct app_kddm_object *obj = NULL;

	spin_lock(&app->lock);
	if (!local_tasks_list_empty(app)) {
		spin_unlock(&app->lock);
		return;
	}
	spin_unlock(&app->lock);

	obj = _kddm_grab_object_no_ft(app_kddm_set, app->app_id);
	if (!obj) /* another process was running delete_app concurrently */
		goto exit;

	r = __delete_local_app(app);
	if (r)
		goto exit_put;

	krgnode_clear(kerrighed_node_id, obj->nodes);

	if (krgnodes_empty(obj->nodes)) {
		DEBUG(DBG_APPLICATION, 1, "DESTROYING APP %ld\n",
		      obj->app_id);
		_kddm_remove_frozen_object(app_kddm_set, obj->app_id);
		goto exit;
	}

exit_put:
	_kddm_put_object(app_kddm_set, obj->app_id);
	DEBUG(DBG_APPLICATION, 1, "End - Appid %ld\n", obj->app_id);
exit:
	return;
}


/*--------------------------------------------------------------------------*/

int create_application(struct task_struct *task)
{
	struct app_struct *app;
	struct app_kddm_object *obj;
	long app_id = task->pid;
	int r = 0;

	obj = _kddm_grab_object(app_kddm_set, app_id);

	if (obj->app_id == app_id) {
		_kddm_put_object(app_kddm_set, app_id);
		r = -EBUSY;
		DEBUG(DBG_APPLICATION, 1, "AppID %ld already exists\n",
		      app_id);
		goto exit;
	}

	obj->app_id = app_id;
	obj->chkpt_sn = 0;

	krgnodes_clear(obj->nodes);
	krgnode_set(kerrighed_node_id, obj->nodes);
	app = new_local_app(app_id);
	if (!app) {
		r = -ENOMEM;
		task->application = NULL;
		_kddm_remove_frozen_object(app_kddm_set, app_id);
		goto exit;
	}

	register_task_to_app(app, task);
	_kddm_put_object(app_kddm_set, app_id);
exit:
	DEBUG(DBG_APPLICATION, 1, "End - Appid %ld\n", app_id);
	return r;
}


static inline task_state_t *__alloc_task_state(void)
{
	task_state_t *t;
	t = kmem_cache_alloc(task_state_cachep, GFP_KERNEL);
	if (!t) {
		t = ERR_PTR(-ENOMEM);
		goto err_mem;
	}
	t->chkpt_result = 0;
err_mem:
	return t;
}

static inline task_state_t *alloc_task_state_from_task(
	struct task_struct *task)
{
	task_state_t *t = __alloc_task_state();

	BUG_ON(!task);

	if (!IS_ERR(t))
		t->task = task;

	return t;
}

task_state_t *alloc_task_state_from_pids(pid_t pid,
					  pid_t tgid,
					  pid_t parent,
					  pid_t real_parent,
					  pid_t real_parent_tgid,
					  pid_t pgrp,
					  pid_t session)
{
	task_state_t *t = __alloc_task_state();;

	if (IS_ERR(t))
		goto err;

	t->task = NULL;
	t->restart.pid = pid;
	t->restart.tgid = tgid;
	t->restart.parent = parent;
	t->restart.real_parent = real_parent;
	t->restart.real_parent_tgid = real_parent_tgid;
	t->restart.pgrp = pgrp;
	t->restart.session = session;

err:
	return t;
}

void free_task_state(task_state_t *t)
{
	kmem_cache_free(task_state_cachep, t);
}

int register_task_to_app(struct app_struct *app,
			 struct task_struct *task)
{
	int r = 0;
	task_state_t *t;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid %ld\n", app->app_id);

	BUG_ON(!app);
	BUG_ON(!task);

	DEBUG(DBG_APPLICATION, 1, "Task %d (%s) registers to application %ld\n",
	      task->pid, task->comm, app->app_id);

	t = alloc_task_state_from_task(task);
	if (IS_ERR(t)) {
		r = PTR_ERR(t);
		goto err;
	}
	t->chkpt_result = PCUS_RUNNING;

	spin_lock(&app->lock);
	task->application = app;
	list_add_tail(&t->next_task, &app->tasks);
	spin_unlock(&app->lock);

	DEBUG(DBG_APPLICATION, 1, "End - Appid %ld\n", app->app_id);
err:
	return r;
}

int register_task_to_appid(long app_id,
			   struct task_struct *task)
{
	int r;
	struct app_struct *app;
	struct app_kddm_object *obj;

	DEBUG(DBG_APPLICATION, 1, "Task %d (%s) registers to APPID %ld\n",
	      task->pid, task->comm, app_id);

	obj = _kddm_grab_object_no_ft(app_kddm_set, app_id);
	BUG_ON(!obj);

	app = find_local_app(app_id);
	if (!app) {
		krgnode_set(kerrighed_node_id, obj->nodes);
		app = new_local_app(app_id);
	}
	r = register_task_to_app(app, task);
	_kddm_put_object(app_kddm_set, app_id);

	return r;
}

void unregister_task_to_app(struct app_struct *app, struct task_struct *task)
{
	struct list_head *tmp, *element;
	task_state_t *t;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid %ld, PID %d\n",
	      app->app_id, task->pid);

	BUG_ON(!app);
	DEBUG(DBG_APPLICATION, 1, "Task %d (%s) UNregisters to application %ld\n",
	      task->pid, task->comm, app->app_id);

	/* remove the task */
	spin_lock(&app->lock);
	task->application = NULL;

	list_for_each_safe(element, tmp, &app->tasks) {
		t = list_entry(element, task_state_t, next_task);
		if (task == t->task) {
			list_del(element);
			spin_unlock(&app->lock);

			free_task_state(t);
			goto exit;
		}
	}
	BUG();

 exit:
	delete_app(app);

	DEBUG(DBG_APPLICATION, 1, "End - PID %d\n", task->pid);
}

/*--------------------------------------------------------------------------*/

void set_task_chkpt_result(struct task_struct *task, int result)
{
	struct app_struct *app;
	struct list_head *tmp, *element;
	task_state_t *t;
	int done_for_all_tasks = 1;

	app = task->application;

	if (!app)
		return;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld, Pid: %d, result: %d\n",
	      app->app_id, task->pid, result);

	list_for_each_safe(element, tmp, &app->tasks) {
		t = list_entry(element, task_state_t, next_task);
		if (task == t->task)
			t->chkpt_result = result;

		if (t->chkpt_result == PCUS_CHKPT_IN_PROGRESS ||
		    t->chkpt_result == PCUS_STOP_IN_PROGRESS)
			done_for_all_tasks = 0;
	}

	if (done_for_all_tasks) {
		DEBUG(DBG_APPLICATION, 3, "Done for all tasks!\n");
		complete(&app->tasks_chkpted);
	}

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld, Pid: %d\n",
	      app->app_id, task->pid);
}

/* before running this method, be sure checkpoints are completed */
int get_local_tasks_chkpt_result(struct app_struct* app)
{
	int r = 0, pcus_result = 0;
	task_state_t *t;

	list_for_each_entry(t, &app->tasks, next_task) {
		pcus_result = t->chkpt_result;
		if (pcus_result == PCUS_RUNNING) {
			/* one process has been forgotten! try again!! */
			return pcus_result;
		} else if (pcus_result == PCUS_STOP_IN_PROGRESS)
			/* Process is zombie !! */
			if (t->task->state == TASK_DEAD)
				return -E_CR_TASKDEAD;

		r = r | pcus_result;
	}

	return r;
}

/*--------------------------------------------------------------------------*/

int export_application(struct epm_action *action,
		       ghost_t *ghost, struct task_struct *task)
{
	int r = 0;
	long app_id = -1;

	BUG_ON(!task);

	/* leave an application if no more checkpointable */
	if (!cap_raised(task->krg_cap_effective, CAP_CHECKPOINTABLE) &&
	    task->application)
		unregister_task_to_app(task->application, task);

	/* Lazy creation of application (step 2/2) */
	/* If process is checkpointable but not in an application
	   and action = REMOTE_CLONE, create the application */
	if (cap_raised(task->krg_cap_effective, CAP_CHECKPOINTABLE) &&
	    !task->application && action->type == EPM_REMOTE_CLONE)
		create_application(task);

	if (!task->application)
		app_id = -1;
	else
		app_id = task->application->app_id;

	DEBUG(DBG_GHOST_MNGMT, 1, "%ld\n", app_id);
	r = ghost_write(ghost, &app_id, sizeof(long));

	return r;
}


int import_application(struct epm_action *action,
		       ghost_t *ghost, struct task_struct *task)
{
	int r;
	long app_id;

	task->application = NULL;

	r = ghost_read(ghost, &app_id, sizeof(long));
	if (r)
		goto out;

	DEBUG(DBG_GHOST_MNGMT, 1, "%ld\n", app_id);

	if (action->type == EPM_CHECKPOINT)
		return 0;

	if (!cap_raised(task->krg_cap_effective, CAP_CHECKPOINTABLE))
		return 0;

	if (app_id == -1) {
		/* this can be done later ... (lazy creation of application) */
		/* create_application(task); */
	} else
		register_task_to_appid(app_id, task);
out:
	return r;
}

void unimport_application(struct epm_action *action,
			  ghost_t *ghost, struct task_struct *task)
{
	if (!task->application)
		return;

	unregister_task_to_app(task->application, task);
}

/*--------------------------------------------------------------------------*/

/* make a local process sleeping (blocking request) */
static inline int __stop_task(struct task_struct *task,
			      const credentials_t *user_creds)
{
	struct siginfo info;
	int signo;
	int retval;

	BUG_ON(!task);
	BUG_ON(task == current);

	if (!can_be_checkpointed(task, user_creds)) {
		retval = -EPERM;
		goto exit;
	}

	retval = krg_action_start(task, EPM_CHECKPOINT);
	if (retval) {
		printk("krg_action_start returns %d (%d %s)\n",
		       retval, task->pid, task->comm);
		goto exit;
	}

	signo = KRG_SIG_CHECKPOINT;
	info.si_errno = 0;
	info.si_pid = 0;
	info.si_uid = 0;
	si_option(info) = CHKPT_ONLY_STOP;

	retval = send_kerrighed_signal(signo, &info, task);
	if (retval)
		BUG();

exit:
	return retval;
}


static inline int __local_stop(struct app_struct *app,
			       const credentials_t *user_creds)
{
	task_state_t *tsk;
	int r = 0;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld\n", app->app_id);

	BUG_ON(list_empty(&app->tasks));

stop_all_running:
	/* Stop all the local processes of the application */
	init_completion(&app->tasks_chkpted);

	list_for_each_entry(tsk, &app->tasks, next_task) {
		if (tsk->chkpt_result == PCUS_RUNNING) {
			tsk->chkpt_result = PCUS_STOP_IN_PROGRESS;
			r = __stop_task(tsk->task, user_creds);
			if (r != 0)
				set_task_chkpt_result(tsk->task, r);
		}
	}
	r = PCUS_STOP_IN_PROGRESS;

	while (r == PCUS_STOP_IN_PROGRESS) {
		printk("*** wait for completion\n");

		wait_for_completion_timeout(&app->tasks_chkpted, 100);
		r = get_local_tasks_chkpt_result(app);

		/* A process may have been forgotten because it is a child of
		   a process which has forked before handling the signal but after
		   looping on each processes of the application */
		if (r == PCUS_RUNNING)
			goto stop_all_running;
	}

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld, r=%d\n", app->app_id, r);
	return r;
}


struct app_stop_msg {
	kerrighed_node_t requester;
	long app_id;
	credentials_t user_creds;
};


static void handle_app_stop(struct rpc_desc *desc, void *_msg, size_t size)
{
	int r;
	struct app_stop_msg *msg = _msg;
	struct app_struct *app;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld\n", msg->app_id);

	app = find_local_app(msg->app_id);
	BUG_ON(!app);
	r = __local_stop(app, &msg->user_creds);

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld - r=%d\n", msg->app_id, r);

	r = rpc_pack_type(desc, r);

	if (r)
		rpc_cancel(desc);
}


int global_stop(struct app_kddm_object *obj, const credentials_t *user_creds)
{
	struct rpc_desc *desc;
	struct app_stop_msg msg;
	int r = 0;

	BUG_ON(!user_creds);

	/* prepare message */
	msg.requester = kerrighed_node_id;
	msg.app_id = obj->app_id;
	msg.user_creds = *user_creds;

	desc = rpc_begin_m(APP_STOP, &obj->nodes);
	r = rpc_pack_type(desc, msg);
	if (r)
		goto err_rpc;

	/* waiting results from the node hosting the application */
	r = app_wait_returns_from_nodes(desc, obj->nodes);

exit:
	rpc_end(desc, 0);

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld\n", obj->app_id);
	return r;
err_rpc:
	rpc_cancel(desc);
	goto exit;
}


/*--------------------------------------------------------------------------*/

/* wake up a local process (blocking request) */
static inline int __continue_task(task_state_t *tsk, int first_run,
				  const credentials_t *user_creds)
{
	int r = 0;
	BUG_ON(!tsk);

	DEBUG(DBG_APPLICATION, 1, "Begin - Pid: %d, first_run: %d\n",
	      tsk->task->pid, first_run);
	DEBUG(DBG_APPLICATION, 1, "Wake up process %d\n", tsk->task->pid);

	if (user_creds && !can_be_checkpointed(tsk->task, user_creds)) {
		r = -EPERM;
		goto exit;
	}

	krg_action_stop(tsk->task, EPM_CHECKPOINT);

	if (!first_run) {
		BUG_ON(tsk->task->state != TASK_UNINTERRUPTIBLE &&
		       tsk->task->state != TASK_INTERRUPTIBLE);
		if (!wake_up_process(tsk->task)) {
			r = -EAGAIN;
			goto exit;
		}
	} else {
		wake_up_new_task(tsk->task, CLONE_VM);
	}

	tsk->chkpt_result = PCUS_RUNNING;

	DEBUG(DBG_APPLICATION, 2,
	      "Synchronization ACK of (%d) : done with error %d -> %ld - %d\n",
	      tsk->task->pid, r, tsk->task->state, task_on_runqueue(tsk->task));

exit:
	DEBUG(DBG_APPLICATION, 1, "End - Pid: %d, first_run: %d, return: %d\n",
	      tsk->task->pid, first_run, r);

	return r;
}


static inline int __local_continue(struct app_struct *app, int first_run,
				   const credentials_t *user_creds)
{
	int r = 0;
	task_state_t *tsk;
	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld, first_run: %d\n",
	      app->app_id, first_run);

	BUG_ON(!app);
	BUG_ON(list_empty(&app->tasks));

	/* make all the local processes of the application going back to
	 * computation */
	list_for_each_entry(tsk, &app->tasks, next_task) {
		r = __continue_task(tsk, first_run, user_creds);
	}

	return r;
}


struct app_continue_msg {
	kerrighed_node_t requester;
	long app_id;
	int first_run;
	int use_creds;
};

static void handle_app_continue(struct rpc_desc *desc, void *_msg, size_t size)
{
	int r;
	credentials_t user_creds;
	struct app_continue_msg *msg = _msg;
	struct app_struct *app = find_local_app(msg->app_id);

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld, first_run: %d\n",
	      msg->app_id, msg->first_run);
	BUG_ON(!app);

	if (msg->use_creds) {
		r = rpc_unpack_type(desc, user_creds);
		if (r)
			goto err;

		r = __local_continue(app, msg->first_run, &user_creds);
	} else
		r = __local_continue(app, msg->first_run, NULL);

	r = rpc_pack_type(desc, r);
	if (r)
		goto err;

	return;

err:
	rpc_cancel(desc);
}


int global_continue(struct app_kddm_object *obj, int first_run,
		    const credentials_t *user_creds)
{
	struct rpc_desc *desc;
	struct app_continue_msg msg;
	int r=0;

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld, first_run: %d\n",
	      obj->app_id, first_run);

	/* prepare message */
	msg.requester = kerrighed_node_id;
	msg.app_id = obj->app_id;
	msg.first_run = first_run;
	if (user_creds)
		msg.use_creds = 1;
	else
		msg.use_creds = 0;

	desc = rpc_begin_m(APP_CONTINUE, &obj->nodes);

	r = rpc_pack_type(desc, msg);
	if (r)
		goto err_rpc;

	if (user_creds) {
		r = rpc_pack_type(desc, *user_creds);
		if (r)
			goto err_rpc;
	}

	/* waiting results from the node hosting the application */
	r = app_wait_returns_from_nodes(desc, obj->nodes);

exit:
	rpc_end(desc, 0);

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld, first_run: %d\n",
	      obj->app_id, first_run);
	return r;

err_rpc:
	rpc_cancel(desc);
	goto exit;
}

/*--------------------------------------------------------------------------*/

static int _kill_process(task_state_t *tsk, int signal,
			 const credentials_t *user_creds)
{
	int r;
	if (!can_be_checkpointed(tsk->task, user_creds)) {
		r = -EPERM;
		goto exit;
	}

	r = kill_pid(tsk->task->pids[PIDTYPE_PID].pid, signal, 1);
	if (r)
		goto exit;

exit:
	return r;
}

static inline int __local_kill(struct app_struct *app, int signal,
			       const credentials_t *user_creds)
{
	int retval = 0;
	int r = 0;
	task_state_t *tsk;
	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld, signal: %d\n",
	      app->app_id, signal);

	BUG_ON(!app);
	BUG_ON(list_empty(&app->tasks));

	/* signal all the local processes of the application */
	list_for_each_entry(tsk, &app->tasks, next_task) {
		retval = _kill_process(tsk, signal, user_creds);
		r = retval | r;
	}

	return r;
}

struct app_kill_msg {
	kerrighed_node_t requester;
	long app_id;
	int signal;
	int use_creds;
};

static void handle_app_kill(struct rpc_desc *desc, void *_msg, size_t size)
{
	int r;
	credentials_t user_creds;
	struct app_kill_msg *msg = _msg;
	struct app_struct *app = find_local_app(msg->app_id);

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld\n", msg->app_id);
	BUG_ON(!app);

	if (msg->use_creds) {
		r = rpc_unpack_type(desc, user_creds);
		if (r)
			goto err;

		r = __local_kill(app, msg->signal, &user_creds);
	} else
		r = __local_kill(app, msg->signal, NULL);

	r = rpc_pack_type(desc, r);
	if (r)
		goto err;

	return;
err:
	rpc_cancel(desc);
}

int global_kill(struct app_kddm_object *obj, int signal,
		const credentials_t *user_creds)
{
	struct rpc_desc *desc;
	struct app_kill_msg msg;
	int r=0;

	BUG_ON(!user_creds);

	DEBUG(DBG_APPLICATION, 1, "Begin - Appid: %ld\n", obj->app_id);

	/* prepare message */
	msg.requester = kerrighed_node_id;
	msg.app_id = obj->app_id;
	msg.signal = signal;
	if (user_creds)
		msg.use_creds = 1;
	else
		msg.use_creds = 0;

	desc = rpc_begin_m(APP_KILL, &obj->nodes);

	r = rpc_pack_type(desc, msg);
	if (r)
		goto err_rpc;

	if (msg.use_creds) {
		r = rpc_pack_type(desc, *user_creds);

		if (r)
			goto err_rpc;
	}

	/* waiting results from the node hosting the application */
	r = app_wait_returns_from_nodes(desc, obj->nodes);

exit:
	rpc_end(desc, 0);

	DEBUG(DBG_APPLICATION, 1, "End - Appid: %ld\n", obj->app_id);
	return r;

err_rpc:
	rpc_cancel(desc);
	goto exit;
}


/*--------------------------------------------------------------------------*
 *                                                                          *
 *          APPLICATION CHECKPOINT SERVER MANAGEMENT                        *
 *                                                                          *
 *--------------------------------------------------------------------------*/

void application_cr_server_init(void)
{
	unsigned long cache_flags = SLAB_PANIC;
#ifdef CONFIG_DEBUG_SLAB
	cache_flags |= SLAB_POISON;
#endif

	DEBUG(DBG_APPLICATION, 1, "Application Checkpoint Server init\n");

	app_struct_table = hashtable_new(5);

	/*------------------------------------------------------------------*/

	register_io_linker(APP_LINKER, &app_io_linker);

	app_kddm_set = create_new_kddm_set(kddm_def_ns, APP_KDDM_ID,
					   APP_LINKER, KDDM_RR_DEF_OWNER,
					   sizeof(struct app_kddm_object),
					   KDDM_LOCAL_EXCLUSIVE);
	if (IS_ERR(app_kddm_set))
		OOM;

	app_kddm_obj_cachep = kmem_cache_create("app_kddm_obj",
                                                sizeof(struct app_kddm_object),
                                                0, cache_flags,
                                                NULL, NULL);
	app_struct_cachep = kmem_cache_create("app_struct",
					      sizeof(struct app_struct),
					      0, cache_flags,
					      NULL, NULL);
	task_state_cachep = kmem_cache_create("task_state_t",
					      sizeof(task_state_t),
					      0, cache_flags,
					      NULL, NULL);

	rpc_register_void(APP_STOP, handle_app_stop, 0);
	rpc_register_void(APP_CONTINUE, handle_app_continue, 0);
	rpc_register_void(APP_KILL, handle_app_kill, 0);

	application_frontier_rpc_init();
	application_checkpoint_rpc_init();
	application_restart_rpc_init();

	DEBUG(DBG_APPLICATION, 1, "Done\n");
}


void application_cr_server_finalize(void)
{
	if (kerrighed_node_id == 0) {
		_destroy_kddm_set(app_kddm_set);
	}
	hashtable_free(app_struct_table);
}
