// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO file descriptors
 *
 * LUO provides the infrastructure necessary to preserve specific types of
 * stateful file descriptors across a kernel live update. The primary goal is to
 * allow workloads, such as virtual machines using vfio, memfd, or iommufd, to
 * retain access to their essential resources without interruption after the
 * underlying kernel is updated.
 *
 * The framework operates based on a two-step "check then act" process involving
 * handler registration and instance tracking:
 *
 * 1. Handler Registration: Kernel modules responsible for specific file types
 * (e.g., memfd, vfio) register a &struct liveupdate_file_handler. This handler
 * provides a set of callbacks for managing the file's lifecycle, most notably:
 *
 *    - can_preserve(): A lightweight, fast check to determine if the handler
 *      is compatible with a given 'struct file'.
 *    - preserve(): The heavyweight operation that performs the actual state
 *      serialization and returns an opaque u64 handle.
 *    - unpreserve(): Cleans up any resources allocated during preserve().
 *    - retrieve(): Reconstructs the file in the new kernel from the
 *      preserved u64 handle.
 *
 * 2. Preservation Flow: When a user requests to preserve a file via the
 * PRESERVE_FD ioctl, the core LUO logic in luo_preserve_file() executes the
 * following sequence:
 *
 *    a. Find Handler: It iterates through all registered handlers, calling
 *       can_preserve() on each to find a compatible one.
 *    b. Serialize State: Once a handler is found, its preserve() callback is
 *       invoked. This function is responsible for performing the bulk of the
 *       state-saving work immediately and returning a u64 handle.
 *    c. Track Instance: An internal &struct luo_file instance is created to
 *       track the 'struct file', the u64 handle returned by preserve(), the
 *       handler that owns it, and the user-provided token. This instance is
 *       added to the parent session's list of files.
 *
 * 3. State Persistence and Restoration: During the global PREPARE phase, the
 * list of luo_file instances is serialized into an array that is preserved
 * across the kexec via KHO. In the new kernel, when the user requests to
 * restore a file, the core logic finds the corresponding handler and calls its
 * retrieve() op, passing it the preserved u64 handle to reconstruct the file.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "luo_internal.h"

/*
 * Serialization for handler-scoped global state.
 */
#define LUO_FH_STATE_COMPATIBLE "luo-fh-states-v1-struct"

struct luo_fh_state_ser_entry {
	char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];
	u64 data;
} __packed;

struct luo_fh_state_ser_header {
	u64 count;
	u64 entries_pa;
} __packed;

static struct luo_fh_state_ser_header *luo_fh_header_in;
static struct luo_fh_state_ser_entry *luo_fh_entries_in;

/* Forward declaration */
static void __luo_fh_global_state_destroy(struct liveupdate_file_handler *fh);

/* Registered files. */
static DECLARE_RWSEM(luo_file_handler_list_rwsem);
static LIST_HEAD(luo_file_handler_list);

/**
 * struct luo_file_ser - Represents the serialized preserves files.
 * @compatible:  File handler compatabile string.
 * @files:   Private data
 * @token:   User provided token for this file
 *
 * If this structure is modified, LUO_SESSION_COMPATIBLE must be updated.
 */
struct luo_file_ser {
	char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];
	u64 data;
	u64 token;
} __packed;

/**
 * struct luo_file - Represents a file descriptor instance preserved
 * across live update.
 * @fh:            Pointer to the &struct liveupdate_file_handler containing
 *                 the implementation of prepare, freeze, cancel, and finish
 *                 operations specific to this file's type.
 * @file:          A pointer to the kernel's &struct file object representing
 *                 the open file descriptor that is being preserved.
 * @private:       A pointer to the liveupdate_file_handler's private data.
 * @data:          Handle to the serialized state of the file.
 * @reclaimed:     Flag indicating whether this preserved file descriptor has
 *                 been successfully 'reclaimed' (e.g., requested via an ioctl)
 *                 by user-space or the owning kernel subsystem in the new
 *                 kernel after the live update.
 * @state:         The current state of file descriptor, it is allowed to
 *                 prepare, freeze, and finish FDs before the global state
 *                 switch.
 * @mutex:         Lock to protect FD state, and allow independently to change
 *                 the FD state compared to global state.
 * @list:          List head for linking this file into its parent session's
 *                 list of preserved files.
 * @token:         The user-provided unique token for this file.
 *
 * This structure holds the necessary callbacks and context for managing a
 * specific open file descriptor throughout the different phases of a live
 * update process. Instances of this structure are typically allocated,
 * populated with file-specific details (&file, &arg, callbacks, compatibility
 * string, token), and linked into a central list managed by the LUO. The
 * data field is used by file handlers to store a handle (physical address of
 * page for example) to the serialized state of the file.
 */
struct luo_file {
	struct liveupdate_file_handler *fh;
	struct file *file;
	void *private;
	u64 data;
	bool reclaimed;
	enum liveupdate_state state;
	struct mutex mutex;
	struct list_head list;
	u64 token;
};

static int luo_file_prepare_one(struct luo_session *session, struct luo_file *h)
{
	int ret = 0;

	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_NORMAL) {
		if (h->fh->ops->prepare) {
			struct liveupdate_file_op_args args = {0};

			args.handler = h->fh;
			args.session = (struct liveupdate_session *)session;
			args.file = h->file;
			args.data = h->data;
			args.private = h->private;

			ret = h->fh->ops->prepare(&args);
			if (!ret)
				h->data = args.data;
		}
		if (!ret)
			h->state = LIVEUPDATE_STATE_PREPARED;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_PREPARED &&
			     h->state != LIVEUPDATE_STATE_FROZEN);
	}

	return ret;
}

static int luo_file_freeze_one(struct luo_session *session, struct luo_file *h)
{
	int ret = 0;

	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_PREPARED) {
		if (h->fh->ops->freeze) {
			struct liveupdate_file_op_args args = {0};

			args.handler = h->fh;
			args.session = (struct liveupdate_session *)session;
			args.file = h->file;
			args.data = h->data;
			args.private = h->private;

			ret = h->fh->ops->freeze(&args);
			if (!ret)
				h->data = args.data;
		}
		if (!ret)
			h->state = LIVEUPDATE_STATE_FROZEN;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_FROZEN);
	}

	return ret;
}

static void luo_file_finish_one(struct luo_session *session, struct luo_file *h)
{
	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_UPDATED) {
		if (h->fh->ops->finish) {
			struct liveupdate_file_op_args args = {0};

			args.handler = h->fh;
			args.session = (struct liveupdate_session *)session;
			args.file = h->file;
			args.data = h->data;
			args.private = h->private;
			args.reclaimed = h->reclaimed;

			h->fh->ops->finish(&args);
		}
		h->state = LIVEUPDATE_STATE_NORMAL;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_NORMAL);
	}
}

static void luo_file_cancel_one(struct luo_session *session, struct luo_file *h)
{
	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_NORMAL)
		return;

	if (WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_PREPARED &&
			 h->state != LIVEUPDATE_STATE_FROZEN)) {
		return;
	}

	if (h->fh->ops->cancel) {
		struct liveupdate_file_op_args args = {0};

		args.handler = h->fh;
		args.session = (struct liveupdate_session *)session;
		args.file = h->file;
		args.data = h->data;
		args.private = h->private;

		h->fh->ops->cancel(&args);
	}

	h->data = 0;
	h->state = LIVEUPDATE_STATE_NORMAL;
}

static void __luo_file_cancel(struct luo_session *session)
{
	struct luo_file *h;

	list_for_each_entry_reverse(h, &session->files_list, list)
		luo_file_cancel_one(session, h);
}

int luo_file_prepare(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	size_t ser_size;
	int ret = 0;
	int i;

	if (!session->count)
		return 0;

	ser_size = session->count * sizeof(struct luo_file_ser);
	ser = luo_contig_alloc_preserve(ser_size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	i = 0;
	list_for_each_entry(luo_file, &session->files_list, list) {
		ret = luo_file_prepare_one(session, luo_file);
		if (ret < 0) {
			pr_err("Prepare failed for session[%s] token[%#0llx] handler[%s] ret[%d]\n",
			       session->name, luo_file->token, luo_file->fh->compatible, ret);
			goto exit_cleanup;
		}

		strscpy(ser[i].compatible, luo_file->fh->compatible,
			sizeof(ser[i].compatible));
		ser[i].data = luo_file->data;
		ser[i].token = luo_file->token;
		i++;
	}

	session->files = __pa(ser);

	return 0;

exit_cleanup:
	__luo_file_cancel(session);
	luo_contig_free_unpreserve(ser, ser_size);

	return ret;
}

int luo_file_freeze(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	size_t ser_size;
	int ret = 0;
	int i;

	if (!session->count)
		return 0;

	if (WARN_ON(!session->files))
		return -EINVAL;

	ser = __va(session->files);

	i = 0;
	list_for_each_entry(luo_file, &session->files_list, list) {
		ret = luo_file_freeze_one(session, luo_file);
		if (ret < 0) {
			pr_err("Freeze failed for session[%s] token[%#0llx] handler[%s] ret[%d]\n",
			       session->name, luo_file->token, luo_file->fh->compatible, ret);
			goto exit_cleanup;
		}
		ser[i].data = luo_file->data;
		i++;
	}

	return 0;

exit_cleanup:
	__luo_file_cancel(session);
	ser_size = session->count * sizeof(struct luo_file_ser);
	luo_contig_free_unpreserve(ser, ser_size);

	return ret;
}

void luo_file_finish(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	size_t ser_size;

	if (!session->count)
		return;

	list_for_each_entry(luo_file, &session->files_list, list)
		luo_file_finish_one(session, luo_file);

	ser_size = session->count * sizeof(struct luo_file_ser);
	ser = __va(session->files);
	luo_contig_free_restore(ser, ser_size);
}

void luo_file_cancel(struct luo_session *session)
{
	struct luo_file_ser *ser;
	size_t ser_size;

	if (!session->count)
		return;

	__luo_file_cancel(session);

	if (session->files) {
		ser = __va(session->files);
		ser_size = session->count * sizeof(struct luo_file_ser);
		luo_contig_free_unpreserve(ser, ser_size);
		session->files = 0;
	}
}

void luo_file_deserialize(struct luo_session *session)
{
	struct luo_file_ser *ser;
	u64 i;

	if (!session->files)
		return;

	guard(rwsem_read)(&luo_file_handler_list_rwsem);
	ser = __va(session->files);
	for (i = 0; i < session->count; i++) {
		struct liveupdate_file_handler *fh;
		bool handler_found = false;
		struct luo_file *luo_file;

		list_for_each_entry(fh, &luo_file_handler_list, list) {
			if (!strcmp(fh->compatible, ser[i].compatible)) {
				handler_found = true;
				break;
			}
		}

		if (!handler_found) {
			luo_restore_fail("No registered handler for compatible '%s'\n",
					 ser[i].compatible);
		}

		luo_file = kzalloc(sizeof(*luo_file),
				   GFP_KERNEL | __GFP_NOFAIL);
		luo_file->fh = fh;
		if (fh->ops->private_size)
			luo_file->private = kzalloc(fh->ops->private_size,
						    GFP_KERNEL | __GFP_NOFAIL);
		luo_file->file = NULL;
		luo_file->data = ser[i].data;
		luo_file->token = ser[i].token;
		luo_file->reclaimed = false;
		mutex_init(&luo_file->mutex);
		luo_file->state = LIVEUPDATE_STATE_UPDATED;
		list_add_tail(&luo_file->list, &session->files_list);
	}
}

/**
 * luo_preserve_file - Find a handler, preserve a file, and register it.
 * @token: Token value for this file descriptor.
 * @fd: file descriptor to be preserved.
 *
 * This function first iterates through registered file handlers using the
 * lightweight can_preserve() op to find a compatible one. Once found, it calls
 * that handler's .preserve() op to perform state serialization and then creates
 * a luo_file instance to track the preserved state within the session.
 *
 * Context: Must be called when LUO is in the 'normal' state.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_preserve_file(struct luo_session *session, u64 token, int fd)
{
	struct liveupdate_file_op_args args = {0};
	struct liveupdate_file_handler *fh;
	struct luo_file *luo_file;
	struct file *file;
	int ret = -ENOENT;

	if (!liveupdate_find_file((struct liveupdate_session *)session,
				  token, NULL))
		return -EEXIST;

	file = fget(fd);
	if (!file)
		return -EBADF;

	guard(rwsem_read)(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh, &luo_file_handler_list, list) {
		if (fh->ops->can_preserve(fh, file)) {
			ret = 0;
			break;
		}
	}

	/* ret is still -ENOENT if no handler was found */
	if (ret)
		goto exit_fput;

	luo_file = kzalloc(sizeof(*luo_file), GFP_KERNEL);
	if (!luo_file) {
		ret = -ENOMEM;
		goto exit_fput;
	}

	luo_file->file = file;
	luo_file->fh = fh;
	luo_file->token = token;
	luo_file->reclaimed = false;
	luo_file->state = LIVEUPDATE_STATE_NORMAL;
	mutex_init(&luo_file->mutex);

	if (fh->ops->private_size) {
		void *private = kzalloc(fh->ops->private_size, GFP_KERNEL);
		if (!private) {
			kfree(luo_file);
			ret = -ENOMEM;
			goto exit_fput;
		}

		luo_file->private = private;
	}

	args.handler = fh;
	args.session = (struct liveupdate_session *)session;
	args.file = file;
	args.private = luo_file->private;
	ret = fh->ops->preserve(&args);
	if (ret) {
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file->private);
		kfree(luo_file);
	} else {
		luo_file->data = args.data;
		list_add_tail(&luo_file->list, &session->files_list);
		atomic_inc(&luo_file->fh->count);
		session->count++;
	}

exit_fput:
	if (ret)
		fput(file);

	return ret;
}

/**
 * luo_unpreserve_file - Unregister the last preserved file from a session.
 * @token: The unique token of the file instance to unregister. Must match the
 *         last file preserved.
 *
 * Enforces a strict LIFO order for un-preservation to correctly manage
 * dependencies. This function checks if the provided @token matches the token
 * of the last file added to the session's preservation list. If it matches,
 * it calls the handler's .unpreserve() op, then removes the file from the
 * session and frees its resources.
 *
 * Context: Must be called to unwind preservation in the reverse order that
 * files were added.
 *
 * Return: 0 on success. -EINVAL if the token does not match the last entry.
 * -ENOENT if the session is empty.
 */
int luo_unpreserve_file(struct luo_session *session, u64 token)
{
	struct luo_file *last_file;

	if (list_empty(&session->files_list))
		return -ENOENT;

	last_file = list_last_entry(&session->files_list, struct luo_file,
				    list);

	/* Check if we attempt to unpreserve out of order */
	if (last_file->token != token) {
		return -EINVAL;
	}

	/* Call the handler to clean up its internal state */
	if (last_file->fh->ops->unpreserve) {
		struct liveupdate_file_op_args args = {0};

		args.handler = last_file->fh;
		args.session = (struct liveupdate_session *)session;
		args.file = last_file->file;
		args.data = last_file->data;
		args.private = last_file->private;
		last_file->fh->ops->unpreserve(&args);
	}

	list_del(&last_file->list);
	session->count--;

	if (last_file->file)
		fput(last_file->file);

	mutex_destroy(&last_file->mutex);
	scoped_guard(rwsem_read, &luo_file_handler_list_rwsem) {
		if (atomic_dec_and_test(&last_file->fh->count))
			__luo_fh_global_state_destroy(last_file->fh);
	}
	kfree(last_file->private);
	kfree(last_file);

	return 0;
}

/**
 * luo_retrieve_file - Restore the next file in the preservation sequence.
 * @token: The unique token of the file instance to retrieve. Must match the
 *         first unrestored file in the session.
 * @filep: Output parameter. On success (return value 0), this will point
 *         to the retrieved "struct file".
 *
 * Enforces a strict FIFO order for restoration to correctly manage
 * dependencies. This function checks if the provided @token matches the token
 * of the first file in the session's preservation list. If it matches and
 * has not been reclaimed, it is restored.
 *
 * Return: 0 on success. -EBUSY if the file has already been restored. -EINVAL
 * if the token is out of order. -ENOENT if the session is empty.
 */
int luo_retrieve_file(struct luo_session *session, u64 token,
		      struct file **filep)
{
	struct liveupdate_file_op_args args = {0};
	struct luo_file *luo_file;
	int ret = 0;

	if (list_empty(&session->files_list))
		return -ENOENT;

	luo_file = list_first_entry(&session->files_list,
				    struct luo_file, list);

	/* Check if we attempt to restore out of order */
	if (luo_file->token != token)
		return -EINVAL;

	if (luo_file->reclaimed)
		return -EBUSY;

	guard(mutex)(&luo_file->mutex);
	if (luo_file->reclaimed)
		return -EBUSY;

	args.handler = luo_file->fh;
	args.session = (struct liveupdate_session *)session;
	args.data = luo_file->data;
	args.private = luo_file->private;
	ret = luo_file->fh->ops->retrieve(&args);
	if (!ret) {
		luo_file->file = args.file;

		/* Get a reference so we can keep this file in LUO */
		get_file(luo_file->file);
		*filep = luo_file->file;
		luo_file->reclaimed = true;
	}

	return ret;
}

void luo_file_unpreserve_all_files(struct luo_session *session)
{
	struct luo_file *last_file;

	while (!list_empty(&session->files_list)) {
		last_file = list_last_entry(&session->files_list,
					    struct luo_file, list);

		if (WARN_ON(luo_unpreserve_file(session, last_file->token)))
			break;
	}
}

void luo_file_unpreserve_unreclaimed_files(struct luo_session *session)
{
	struct luo_file *h, *tmp;

	/*
	 * Iterate safely as we will be removing entries from the list. This is a
	 * cleanup operation, so we remove any unreclaimed file regardless of
	 * its position.
	 */
	list_for_each_entry_safe(h, tmp, &session->files_list, list) {
		if (!h->reclaimed) {
			pr_err("Unpreserving unreclaimed file, session[%s] token[%#0llx] handler[%s]\n",
			       session->name, h->token, h->fh->compatible);

			list_del(&h->list);
			session->count--;

			if (h->file)
				fput(h->file);

			mutex_destroy(&h->mutex);
			scoped_guard(rwsem_read, &luo_file_handler_list_rwsem) {
				if (atomic_dec_and_test(&h->fh->count))
					__luo_fh_global_state_destroy(h->fh);
			}
			kfree(h->private);
			kfree(h);
		}
	}
}

long luo_file_query(struct luo_session *session,
		    struct liveupdate_file_handler *h,
		    struct liveupdate_fd *fds)
{
	struct luo_file *luo_file;
	long found = 0;

	list_for_each_entry(luo_file, &session->files_list, list) {
		if (luo_file->fh == h) {
			fds[found].session_name = session->name;
			fds[found].token = luo_file->token;
			fds[found].data = luo_file->data;
			found++;
		}
	}

	return found;
}

/**
 * liveupdate_register_file_handler - Register a file handler with LUO.
 * @fh: Pointer to a caller-allocated &struct liveupdate_file_handler.
 * The caller must initialize this structure, including a unique
 * 'compatible' string and a valid 'fh' callbacks. This function adds the
 * handler to the global list of supported file handlers.
 *
 * Context: Typically called during module initialization for file types that
 * support live update preservation.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_register_file_handler(struct liveupdate_file_handler *fh)
{
	struct liveupdate_file_handler *fh_iter;

	guard(rwsem_read)(&luo_state_rwsem);
	if (!liveupdate_state_normal() && !liveupdate_state_updated())
		return -EBUSY;

	guard(rwsem_write)(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh_iter, &luo_file_handler_list, list) {
		if (!strcmp(fh_iter->compatible, fh->compatible)) {
			pr_err("File handler registration failed: Compatible string '%s' already registered.\n",
			       fh->compatible);
			return -EEXIST;
		}
	}

	if (!try_module_get(fh->ops->owner))
		return -EAGAIN;

	mutex_init(&fh->global_state_lock);
	fh->global_state_obj = NULL;
	fh->global_state_handle = 0;
	INIT_LIST_HEAD(&fh->list);
	atomic_set(&fh->count, 0);
	list_add_tail(&fh->list, &luo_file_handler_list);

	if (liveupdate_state_updated() && luo_fh_entries_in) {
		int i;

		for (i = 0; i < luo_fh_header_in->count; i++) {
			if (!strcmp(luo_fh_entries_in[i].compatible, fh->compatible)) {
				fh->global_state_handle = luo_fh_entries_in[i].data;
				break;
			}
		}
	}

	return 0;
}

/**
 * liveupdate_unregister_file - Unregister a file handler.
 * @fh: Pointer to the specific &struct liveupdate_file_handler instance
 * that was previously returned by or passed to
 * liveupdate_register_file_handler.
 *
 * Removes the specified handler instance @fh from the global list of
 * registered file handlers. This function only removes the entry from the
 * list; it does not free the memory associated with @fh itself. The caller
 * is responsible for freeing the structure memory after this function returns
 * successfully.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_unregister_file_handler(struct liveupdate_file_handler *fh)
{
	guard(rwsem_read)(&luo_state_rwsem);
	if (!liveupdate_state_normal() && !liveupdate_state_updated())
		return -EBUSY;

	guard(rwsem_write)(&luo_file_handler_list_rwsem);
	if (atomic_read(&fh->count)) {
		pr_warn("Unable to unregister file handler, files are preserved\n");
		return -EBUSY;
	}

	list_del_init(&fh->list);
	module_put(fh->ops->owner);

	return 0;
}

/**
 * liveupdate_find_file - Search for a preserved file by its token.
 * @sn:      The session handle to search within.
 * @token:   The token to search for.
 * @filep:   Optional Output parameter. On success, this will point to the found
 *           'struct file *'.
 *
 * Searches the session's list of preserved files for a matching token and
 * returns a pointer to the associated file object.
 *
 * Return: 0 if a matching file is found, -ENOENT otherwise.
 */
int liveupdate_find_file(struct liveupdate_session *sn, u64 token,
			 struct file **filep)
{
	/* Cast the public opaque handle to the internal type */
	struct luo_session *session = (struct luo_session *)sn;
	struct luo_file *iter;

	list_for_each_entry(iter, &session->files_list, list) {
		if (iter->token == token) {
			if (filep)
				*filep = iter->file;
			return 0;
		}
	}

	return -ENOENT;
}

void *liveupdate_fh_global_state_get(struct liveupdate_file_handler *h)
{
	int ret = 0;

	mutex_lock(&h->global_state_lock);

	if (h->global_state_obj)
		return h->global_state_obj;

	if (liveupdate_state_updated()) {
		if (!h->ops->global_state_restore) {
			ret = -EOPNOTSUPP;
			goto err_unlock;
		}
		ret = h->ops->global_state_restore(h, h->global_state_handle,
						   &h->global_state_obj);
	} else {
		if (!h->ops->global_state_create) {
			ret = -EOPNOTSUPP;
			goto err_unlock;
		}
		ret = h->ops->global_state_create(h, &h->global_state_obj,
						  &h->global_state_handle);
	}

	if (ret || !h->global_state_obj) {
		h->global_state_obj = NULL;
		h->global_state_handle = 0;
		if (!ret)
			ret = -ENODATA;
		goto err_unlock;
	}

	return h->global_state_obj;

err_unlock:
	mutex_unlock(&h->global_state_lock);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(liveupdate_fh_global_state_get);

void liveupdate_fh_global_state_put(struct liveupdate_file_handler *h)
{
	mutex_unlock(&h->global_state_lock);
}
EXPORT_SYMBOL_GPL(liveupdate_fh_global_state_put);

static void __luo_fh_global_state_destroy(struct liveupdate_file_handler *fh)
{
	mutex_lock(&fh->global_state_lock);
	if (fh->global_state_obj) {
		if (fh->ops->global_state_destroy)
			fh->ops->global_state_destroy(fh, fh->global_state_obj);
		fh->global_state_obj = NULL;
		fh->global_state_handle = 0;
	}
	mutex_unlock(&fh->global_state_lock);
}

static int luo_fh_global_state_prepare(struct liveupdate_subsystem *h,
				       u64 *data)
{
	struct liveupdate_file_handler *fh;
	struct luo_fh_state_ser_header *header;
	struct luo_fh_state_ser_entry *entries;
	size_t entries_size;
	u64 count = 0;
	int i = 0;

	down_read(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh, &luo_file_handler_list, list) {
		if (atomic_read(&fh->count) > 0 && fh->global_state_handle)
			count++;
	}
	up_read(&luo_file_handler_list_rwsem);

	if (count == 0) {
		*data = 0;
		return 0;
	}

	entries_size = count * sizeof(*entries);
	entries = luo_contig_alloc_preserve(entries_size);
	if (IS_ERR(entries))
		return PTR_ERR(entries);

	header = luo_contig_alloc_preserve(sizeof(*header));
	if (IS_ERR(header)) {
		luo_contig_free_unpreserve(entries, entries_size);
		return PTR_ERR(header);
	}

	down_read(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh, &luo_file_handler_list, list) {
		if (atomic_read(&fh->count) > 0 && fh->global_state_handle) {
			strscpy(entries[i].compatible, fh->compatible, sizeof(entries[i].compatible));
			entries[i].data = fh->global_state_handle;
			i++;
		}
	}
	up_read(&luo_file_handler_list_rwsem);

	header->count = count;
	header->entries_pa = __pa(entries);
	*data = __pa(header);

	return 0;
}

static void luo_fh_global_state_boot(struct liveupdate_subsystem *h, u64 data)
{
	if (!data)
		return;

	luo_fh_header_in = __va(data);
	luo_fh_entries_in = __va(luo_fh_header_in->entries_pa);
}

static const struct liveupdate_subsystem_ops luo_fh_state_subsys_ops = {
	.prepare = luo_fh_global_state_prepare,
	.boot = luo_fh_global_state_boot,
	.owner = THIS_MODULE,
};

static struct liveupdate_subsystem luo_fh_state_subsys = {
	.ops = &luo_fh_state_subsys_ops,
	.name = LUO_FH_STATE_COMPATIBLE,
};

static int __init luo_file_init(void)
{
	if (!liveupdate_enabled())
		return 0;

	return liveupdate_register_subsystem(&luo_fh_state_subsys);
}
late_initcall(luo_file_init);
