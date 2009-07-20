#include <linux/types.h>

#ifndef CHECKPOINT_TYPES_H
#define CHECKPOINT_TYPES_H

#define E_CR_APPBUSY     1000
#define E_CR_PIDBUSY     1001
#define E_CR_TASKDEAD    1002
#define E_CR_BADDATA     1003

typedef enum {
	FROM_APPID,
	FROM_PID
} type_ckpt_t;

struct checkpoint_info
{
	long app_id;

	type_ckpt_t type;

	int chkpt_sn;
	int result;

	int signal;
};

#define GET_RESTART_CMD_PTS 1

struct restart_request
{
	long app_id;
	int chkpt_sn;
	int flags;
};

struct app_userdata_request
{
	long app_id;
	type_ckpt_t type;
	__u64 user_data;
};

#endif
