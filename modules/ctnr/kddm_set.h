/** KDDM kddm interface.
 *  @file kddm_set.h
 *
 *  Definition of KDDM set interface.
 *  @author Renaud Lottiaux
 */

#ifndef __KDDM_SET__
#define __KDDM_SET__

#include <linux/socket.h>
#include <kerrighed/sys/types.h>
#include <kerrighed/krginit.h>
#include <kerrighed/krgsyms.h>

#include "kddm_types.h"
#include "name_space.h"
#include "kddm_tree.h"
#include "io_linker.h"



/*--------------------------------------------------------------------------*
 *                                                                          *
 *                             MACRO CONSTANTS                              *
 *                                                                          *
 *--------------------------------------------------------------------------*/



/* KDDM set state */
enum
  {
    KDDM_SET_UNINITIALIZED,
    KDDM_SET_NEED_LOOKUP,
    KDDM_SET_INVALID,
    KDDM_SET_LOCKED,
    KDDM_SET_READY,
  };



#define KDDM_ALLOC_STRUCT 1
#define KDDM_CHECK_UNIQUE 2


/** Return the manager id of the given kddm set */
#define KDDM_SET_MGR(set) ((kerrighed_node_t)(set->id >> UNIQUE_ID_NODE_SHIFT))

#define MAX_PRIVATE_DATA_SIZE (PAGE_SIZE-sizeof(msg_kddm_set_t))

/** Default size of a kddm set hash table */
#define KDDM_SET_HASH_TABLE_SIZE 1024

/** Default size for readahead windows */
#define DEFAULT_READAHEAD_WINDOW_SIZE 8

/* Kddm set with round robin distributed default owner */
#define KDDM_RR_DEF_OWNER ((kerrighed_node_t)(KERRIGHED_MAX_NODES + 1))

/* Kddm set with a custom default owner policy */
#define KDDM_CUSTOM_DEF_OWNER ((kerrighed_node_t)(KERRIGHED_MAX_NODES + 2))

/* MUST ALWAYS BE THE LAST ONE and equal to the highest possible value */
#define KDDM_MAX_DEF_OWNER ((kerrighed_node_t)(KERRIGHED_MAX_NODES + 2))

/* Kddm set id reserved for internal system usage (sys_kddm_ns name space). */
enum
  {
    KDDM_SET_UNUSED,                  //  0
    KDFS_FILE_KDDM__ID,	              //  1
    KDFS_INODE_KDDM_ID,               //  2
    KDFS_DENTRY_KDDM_ID,              //  3
    TASK_KDDM_ID,                     //  4
    SIGNAL_STRUCT_KDDM_ID,            //  5
    SIGHAND_STRUCT_KDDM_ID,           //  6
    STATIC_NODE_INFO_KDDM_ID,         //  7
    STATIC_CPU_INFO_KDDM_ID,          //  8
    DYNAMIC_NODE_INFO_KDDM_ID,        //  9
    DYNAMIC_CPU_INFO_KDDM_ID,         // 10
    MEMORY_NODE_INFO_KDDM_ID,         // 11
    STREAM_KDDM_ID,                   // 12
    SOCKET_KDDM_ID,                   // 13
    APP_KDDM_ID,                      // 14
    FUTEX_KDDM_ID,                    // 15
    SHMID_KDDM_ID,                    // 16
    SHMKEY_KDDM_ID,                   // 17
    SHMMAP_KDDM_ID,                   // 18
    SEMARRAY_KDDM_ID,                 // 19
    SEMKEY_KDDM_ID,                   // 20
    SEMMAP_KDDM_ID,                   // 21
    SEMUNDO_KDDM_ID,                  // 22
    MSG_KDDM_ID,                      // 23
    MSGKEY_KDDM_ID,                   // 24
    MSGMAP_KDDM_ID,                   // 25
    MSGMASTER_KDDM_ID,                // 26
    DSTREAM_KDDM_ID,                  // 27
    DSTREAM_SOCKET_BASE,              // 28
    DSTREAM_SOCKET_MAX = DSTREAM_SOCKET_BASE + AF_MAX, //  + 32 = 60
    DSTREAM_PIPE_KDDM_ID,             // 61
    PID_KDDM_ID,                      // 62
    CHILDREN_KDDM_ID,                 // 63
    DVFS_FILE_STRUCT_KDDM_ID,         // 64
    GLOBAL_LOCK_KDDM_SET_ID,	      // 65
    GLOBAL_CONFIG_KDDM_SET_ID,        // 66
    KDDM_TEST4_DIST,                  // 67
    KDDM_TEST4_LOC,                   // 68
    KDDM_TEST4096,                    // 69
    MM_STRUCT_KDDM_ID,                // 70
    MIN_KDDM_ID,           /* MUST always be the last one */
  };

#define DSTREAM_UNIX_KDDM_ID (DSTREAM_SOCKET_BASE+AF_UNIX)
#define DSTREAM_INET_KDDM_ID (DSTREAM_SOCKET_BASE+AF_INET)



/*--------------------------------------------------------------------------*
 *                                                                          *
 *                                  TYPES                                   *
 *                                                                          *
 *--------------------------------------------------------------------------*/



/** kddm set manager message type.
 *  Used to store informations to sent to the KDDM set manager server.
 */
typedef struct {
	int kddm_ns;               /**< KDDM name space identifier */
	kddm_set_id_t kddm_set_id; /**< KDDM set identifier */
	unsigned long flags;       /**< Kddm Set flags */
	kerrighed_node_t link;     /**< Node linked to the kddm set */
	int obj_size;              /**< Size of objects stored in kddm set */
	iolinker_id_t linker_id;   /**< Identifier of the io linker  */
	unsigned long data_size;   /**< Size of set private data to receive */
	krgsyms_val_t set_ops;     /**< KDDM set operations struct ID */
	char private_data[1];
} msg_kddm_set_t;



typedef struct {
	int ns_id;                   /**< KDDM name space identifier */
	kddm_set_id_t set_id;        /**< KDDM set identifier */
} kddm_id_msg_t;



/*--------------------------------------------------------------------------*
 *                                                                          *
 *                              EXTERN FUNCTIONS                            *
 *                                                                          *
 *--------------------------------------------------------------------------*/



void kddm_set_init(void);
void kddm_set_finalize(void);

struct kddm_set *__create_new_kddm_set(struct kddm_ns *ns,
				       kddm_set_id_t kddm_set_id,
				       struct kddm_set_ops *set_ops,
				       void *tree_init_data,
				       iolinker_id_t linker_id,
				       kerrighed_node_t def_owner,
				       int obj_size,
				       void *private_data,
				       unsigned long data_size,
				       unsigned long flags);

static inline struct kddm_set *_create_new_kddm_set(struct kddm_ns *ns,
				       kddm_set_id_t kddm_set_id,
				       iolinker_id_t linker_id,
				       kerrighed_node_t def_owner,
				       int obj_size,
				       void *private_data,
				       unsigned long data_size,
				       unsigned long flags)
{
	return (struct kddm_set *) __create_new_kddm_set(ns, kddm_set_id,
						 &kddm_tree_set_ops,
						 _nlevels_kddm_tree_init_data,
						 linker_id, def_owner,
						 obj_size, private_data,
						 data_size, flags);
}

static inline struct kddm_set *create_new_kddm_set(struct kddm_ns *ns,
				       kddm_set_id_t kddm_set_id,
				       iolinker_id_t linker_id,
				       kerrighed_node_t def_owner,
				       int obj_size,
				       unsigned long flags)
{
	return (struct kddm_set *) __create_new_kddm_set(ns, kddm_set_id,
						 &kddm_tree_set_ops,
						 _nlevels_kddm_tree_init_data,
						 linker_id, def_owner,
						 obj_size, NULL, 0, flags);
}

int _destroy_kddm_set(struct kddm_set * kddm_set);
int destroy_kddm_set(struct kddm_ns *ns, kddm_set_id_t set_id);

struct kddm_set *_find_get_kddm_set(struct kddm_ns *ns,
				    kddm_set_id_t kddm_set_id);

struct kddm_set *find_get_kddm_set(int ns_id,
				   kddm_set_id_t set_id);

struct kddm_set *generic_local_get_kddm_set(int ns_id,
					     kddm_set_id_t set_id,
					     int init_state,
					     int flags);

struct kddm_set *_generic_local_get_kddm_set(struct kddm_ns *ns,
					     kddm_set_id_t set_id,
					     int init_state,
					     int flags);

/** Different flavors of the get_kddm_set function */

static inline struct kddm_set *_local_get_kddm_set(struct kddm_ns *ns,
						   kddm_set_id_t set_id)
{
	return _generic_local_get_kddm_set(ns, set_id, 0, 0);
}

static inline struct kddm_set *_local_get_alloc_kddm_set(struct kddm_ns *ns,
							 kddm_set_id_t set_id,
							 int init_state)
{
	return _generic_local_get_kddm_set(ns, set_id, init_state,
					   KDDM_ALLOC_STRUCT);
}

static inline struct kddm_set *local_get_kddm_set(int ns_id,
						  kddm_set_id_t set_id)
{
	return generic_local_get_kddm_set(ns_id, set_id, 0, 0);
}

static inline struct kddm_set *local_get_alloc_kddm_set(int ns_id,
							kddm_set_id_t set_id,
							int init_state)
{
	return generic_local_get_kddm_set(ns_id, set_id, init_state,
					  KDDM_ALLOC_STRUCT);
}

static inline struct kddm_set *_local_get_alloc_unique_kddm_set(
	                                          struct kddm_ns *ns,
						  kddm_set_id_t set_id,
						  int init_state)
{
	return _generic_local_get_kddm_set(ns, set_id, init_state,
					   KDDM_ALLOC_STRUCT |
					   KDDM_CHECK_UNIQUE);

}

void put_kddm_set(struct kddm_set *set);

int _increment_kddm_set_usage(struct kddm_set * kddm_set);
int increment_kddm_set_usage(struct kddm_ns *ns, kddm_set_id_t set_id);
int _decrement_kddm_set_usage(struct kddm_set * kddm_set);
int decrement_kddm_set_usage(struct kddm_ns *ns, kddm_set_id_t set_id);

#endif // __KDDM_NS__
