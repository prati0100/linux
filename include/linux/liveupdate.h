/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_LIVEUPDATE_H
#define _LINUX_LIVEUPDATE_H

#include <linux/bug.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <uapi/linux/liveupdate.h>

struct liveupdate_subsystem;
struct liveupdate_file_handler;
struct liveupdate_session;
struct file;

/**
 * struct liveupdate_file_op_args - Arguments for file operation callbacks.
 *
 * This structure bundles all parameters for the file operation callbacks.
 * The 'data' and 'file' fields are used for both input and output.
 *
 * @handler:   The file handler being called.
 * @session:   The session this file belongs to.
 * @reclaimed: The reclaimed status for the 'finish' operation.
 * @private:   The private data for the file. Used to hold runtime state that is
 *             not preserved. Set by the callback.
 * @data:      The opaque u64 handle.
 *             - preserve/prepare/freeze may update this field.
 * @file:      The file object.
 *             - For retrieve: [OUT] The callback sets this to the new file.
 *             - For other ops: [IN] The caller sets this to the file being
 *               operated on.
 */
struct liveupdate_file_op_args {
	struct liveupdate_file_handler *handler;
	struct liveupdate_session *session;
	bool reclaimed;
	void *private;
	u64 data;
	struct file *file;
};

/**
 * struct liveupdate_file_ops - Callbacks for live-updatable files.
 *
 * All operations (except can_preserve) receive a pointer to a
 * 'struct liveupdate_file_op_args' containing the necessary context.
 *
 * @can_preserve:         Required. Lightweight check to see if this handler is
 *                        compatible with the given file.
 * @preserve:             Required. Performs heavy state-saving for the file.
 * @unpreserve:           Optional. Cleans up any resources allocated by
 *                        @preserve.
 * @prepare:              Optional. Lightweight final checks during the global
 *                        PREPARE.
 * @freeze:               Optional. Final actions just before kernel transition.
 * @cancel:               Optional. Cleans up after a global abort.
 * @finish:               Optional. Final cleanup in the new kernel.
 * @retrieve:             Required. Restores the file in the new kernel.
 * @global_state_create:  Optional. Creates a handler-scoped global state
 *                        object.
 * @global_state_restore: Optional. Restores the global state object.
 * @global_state_destroy: Optional. Destroys the global state object.
 * @owner:                Module reference
 * @private_size:         Optional. Size of file private data.
 */
struct liveupdate_file_ops {
	bool (*can_preserve)(struct liveupdate_file_handler *handler,
			     struct file *file);
	int (*preserve)(struct liveupdate_file_op_args *args);
	void (*unpreserve)(struct liveupdate_file_op_args *args);
	int (*prepare)(struct liveupdate_file_op_args *args);
	int (*freeze)(struct liveupdate_file_op_args *args);
	void (*cancel)(struct liveupdate_file_op_args *args);
	void (*finish)(struct liveupdate_file_op_args *args);
	int (*retrieve)(struct liveupdate_file_op_args *args);
	int (*global_state_create)(struct liveupdate_file_handler *h,
				   void **obj, u64 *data_handle);
	int (*global_state_restore)(struct liveupdate_file_handler *h,
				    u64 data, void **obj);
	void (*global_state_destroy)(struct liveupdate_file_handler *h,
				     void *obj);
	struct module *owner;
	unsigned long private_size;
};

/* The max size is set so it can be reliably used during in serialization */
#define LIVEUPDATE_HNDL_COMPAT_LENGTH	48

/**
 * struct liveupdate_file_handler - Represents a handler for a live-updatable
 * file type.
 * @ops:                Callback functions
 * @compatible:         The compatibility string (e.g., "memfd-v1", "vfiofd-v1")
 *                      that uniquely identifies the file type this handler
 *                      supports. This is matched against the compatible string
 *                      associated with individual &struct liveupdate_file
 *                      instances.
 * @list:               Used for linking this handler instance into a global
 *                      list of registered file handlers.
 * @count:              Atomic counter of number of files that are preserved and
 *                      use this handler.
 * global_state_lock:   Protects access to global state data.
 * global_state_obj:    Global state object that can be accessed by various
 *                      subsystems that need to keep state bound to the life
 *                      cycle of FDs of a specific type.
 * global_stage_handle: Used to pass global state from current kernel to next
 *                      kernel.
 *
 * Modules that want to support live update for specific file types should
 * register an instance of this structure. LUO uses this registration to
 * determine if a given file can be preserved and to find the appropriate
 * operations to manage its state across the update.
 */
struct liveupdate_file_handler {
	const struct liveupdate_file_ops *ops;
	const char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];
	struct list_head list;
	atomic_t count;
	struct mutex global_state_lock;
	void *global_state_obj;
	u64 global_state_handle;
};

/**
 * struct liveupdate_fd - Describes a preserved file descriptor.
 * @session_name: The name of the session this FD belongs to.
 * @token:        The user-provided token for this FD.
 * @data:         The private u64 data payload saved by the handler's
 *                .preserve() callback.
 */
struct liveupdate_fd {
	const char *session_name;
	u64 token;
	u64 data;
};

/**
 * struct liveupdate_subsystem_ops - LUO events callback functions
 * @prepare:      Optional. Called during LUO prepare phase. Should perform
 *                preparatory actions and can store a u64 handle/state
 *                via the 'data' pointer for use in later callbacks.
 *                Return 0 on success, negative error code on failure.
 * @freeze:       Optional. Called during LUO freeze event (before actual jump
 *                to new kernel). Should perform final state saving actions and
 *                can update the u64 handle/state via the 'data' pointer. Retur:
 *                0 on success, negative error code on failure.
 * @cancel:       Optional. Called if the live update process is canceled after
 *                prepare (or freeze) was called. Receives the u64 data
 *                set by prepare/freeze. Used for cleanup.
 * @boot:         Optional. Call durng boot post live update. This callback is
 *                done when subsystem register during live update.
 * @finish:       Optional. Called after the live update is finished in the new
 *                kernel.
 *                Receives the u64 data set by prepare/freeze. Used for cleanup.
 * @owner:        Module reference
 */
struct liveupdate_subsystem_ops {
	int (*prepare)(struct liveupdate_subsystem *handle, u64 *data);
	int (*freeze)(struct liveupdate_subsystem *handle, u64 *data);
	void (*cancel)(struct liveupdate_subsystem *handle, u64 data);
	void (*boot)(struct liveupdate_subsystem *handle, u64 data);
	void (*finish)(struct liveupdate_subsystem *handle, u64 data);
	struct module *owner;
};

/**
 * struct liveupdate_subsystem - Represents a subsystem participating in LUO
 * @ops:          Callback functions
 * @name:         Unique name identifying the subsystem.
 * @list:         List head used internally by LUO. Should not be modified by
 *                caller after registration.
 * @private_data: For LUO internal use, cached value of data field.
 */
struct liveupdate_subsystem {
	const struct liveupdate_subsystem_ops *ops;
	const char *name;
	struct list_head list;
	u64 private_data;
};

#ifdef CONFIG_LIVEUPDATE

/* Return true if live update orchestrator is enabled */
bool liveupdate_enabled(void);

/* Called during reboot to tell participants to complete serialization */
int liveupdate_reboot(void);

/*
 * Return true if machine is in updated state (i.e. live update boot in
 * progress)
 */
bool liveupdate_state_updated(void);

/*
 * Return true if machine is in normal state (i.e. no live update in progress).
 */
bool liveupdate_state_normal(void);

enum liveupdate_state liveupdate_get_state(void);

int liveupdate_register_subsystem(struct liveupdate_subsystem *h);
int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h);
int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h, u64 *data);

int liveupdate_register_file_handler(struct liveupdate_file_handler *h);
int liveupdate_unregister_file_handler(struct liveupdate_file_handler *h);

int liveupdate_find_file(struct liveupdate_session *sn, u64 token,
			 struct file **filep);

void *liveupdate_fh_global_state_get(struct liveupdate_file_handler *h);
void liveupdate_fh_global_state_put(struct liveupdate_file_handler *h);

int liveupdate_fd_data_query(struct liveupdate_file_handler *h,
                             struct liveupdate_fd *fds, long count);

#else /* CONFIG_LIVEUPDATE */

static inline int liveupdate_reboot(void)
{
	return 0;
}

static inline bool liveupdate_enabled(void)
{
	return false;
}

static inline bool liveupdate_state_updated(void)
{
	return false;
}

static inline bool liveupdate_state_normal(void)
{
	return true;
}

static inline enum liveupdate_state liveupdate_get_state(void)
{
	return LIVEUPDATE_STATE_NORMAL;
}

static inline int liveupdate_register_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h,
						u64 *data)
{
	return -ENODATA;
}

static inline int liveupdate_register_file_handler(struct liveupdate_file_handler *h)
{
	return 0;
}

static inline int liveupdate_unregister_file_handler(struct liveupdate_file_handler *h)
{
	return 0;
}

static inline int liveupdate_find_file(struct liveupdate_session *sn, u64 token,
				       struct file **filep)
{
	return -ENOENT;
}

static inline void *liveupdate_fh_global_state_get(struct liveupdate_file_handler *h)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void liveupdate_fh_global_state_put(struct liveupdate_file_handler *h)
{
}

static inline int liveupdate_fd_data_query(struct liveupdate_file_handler *h,
					   struct liveupdate_fd *fds, long count)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
