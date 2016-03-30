/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "handle.h"
#include "message.h"
#include "peer.h"
#include "pool.h"
#include "queue.h"
#include "user.h"

/**
 * bus1_message_new() - allocate new message
 * @n_bytes:		number of bytes to transmit
 * @n_files:		number of files to pre-allocate
 * @n_handles:		number of handles to pre-allocate
 * @silent:		is this a silent message?
 *
 * This allocates a new, unused message for free use to the caller. Storage for
 * files and handles is (partially) pre-allocated. The number of embedded
 * handles is capped, so in case many handles are passed more memory will have
 * to be allocated later.
 *
 * Return: Pointer to new message, ERR_PTR on failure.
 */
struct bus1_message *bus1_message_new(size_t n_bytes,
				      size_t n_files,
				      size_t n_handles,
				      bool silent)
{
	struct bus1_message *message;
	size_t base_size, fds_size;

	base_size = ALIGN(sizeof(*message) +
			  bus1_handle_batch_inline_size(n_handles), 8);
	fds_size = n_files * sizeof(struct file *);

	message = kmalloc(base_size + fds_size, GFP_KERNEL);
	if (!message)
		return ERR_PTR(-ENOMEM);

	bus1_queue_node_init(&message->qnode,
			     silent ? BUS1_QUEUE_NODE_MESSAGE_SILENT :
				      BUS1_QUEUE_NODE_MESSAGE_NORMAL);
	message->data.destination = 0;
	message->data.uid = -1;
	message->data.gid = -1;
	message->data.pid = 0;
	message->data.tid = 0;
	message->data.offset = BUS1_OFFSET_INVALID;
	message->data.n_bytes = n_bytes;
	message->data.n_handles = n_handles;
	message->data.n_fds = n_files;
	message->transaction.next = NULL;
	message->transaction.handle = NULL;
	message->transaction.raw_peer = NULL;
	message->user = NULL;
	message->slice = NULL;
	message->files = (void *)((u8 *)message + base_size);
	bus1_handle_inflight_init(&message->handles, n_handles);
	memset(message->files, 0, n_files * sizeof(*message->files));

	return message;
}

/**
 * bus1_message_free() - destroy a message
 * @message:		message to destroy, or NULL
 *
 * This deallocates, destroys, and frees a message that was previously created
 * via bus1_message_new(). The caller must take care to unlink the message from
 * any queues before calling this. Furthermore, quotas must be handled before
 * as well.
 *
 * Return: NULL is returned.
 */
struct bus1_message *bus1_message_free(struct bus1_message *message)
{
	size_t i;

	if (!message)
		return NULL;

	WARN_ON(message->slice);
	WARN_ON(message->user);
	WARN_ON(message->transaction.raw_peer);
	WARN_ON(message->transaction.handle);
	WARN_ON(message->transaction.next);

	for (i = 0; i < message->data.n_fds; ++i)
		if (message->files[i])
			fput(message->files[i]);

	bus1_handle_inflight_destroy(&message->handles);
	bus1_queue_node_destroy(&message->qnode);
	kfree_rcu(message, qnode.rcu);

	return NULL;
}

/**
 * bus1_message_allocate_locked() - allocate pool slice for message payload
 * @message:		message to allocate slice for
 * @peer_info:		destination peer
 * @user:		user to account in-flight resources on
 *
 * Allocate a pool slice for the given message, and charge the quota of the
 * given user for all the associated in-flight resources. The peer_info lock
 * must be held.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_message_allocate_locked(struct bus1_message *message,
				 struct bus1_peer_info *peer_info,
				 struct bus1_user *user)
{
	struct bus1_pool_slice *slice;
	size_t slice_size;
	int r;

	lockdep_assert_held(&peer_info->lock);

	if (WARN_ON(message->user || message->slice))
		return -EINVAL;

	r = bus1_user_quota_charge(peer_info, user,
				   message->data.n_bytes,
				   message->data.n_handles,
				   message->data.n_fds);
	if (r < 0)
		return r;

	/* cannot overflow as all of those are limited */
	slice_size = ALIGN(message->data.n_bytes, 8) +
		     ALIGN(message->data.n_handles * sizeof(u64), 8) +
		     ALIGN(message->data.n_fds + sizeof(int), 8);

	slice = bus1_pool_alloc(&peer_info->pool, slice_size);
	if (IS_ERR(slice)) {
		bus1_user_quota_discharge(peer_info, user,
					  message->data.n_bytes,
					  message->data.n_handles,
					  message->data.n_fds);
		return PTR_ERR(slice);
	}

	message->user = bus1_user_ref(user);
	message->slice = slice;
	message->data.offset = slice->offset;
	return 0;
}

/**
 * bus1_message_deallocate_locked() - deallocate pool slice for message payload
 * @message:		message to deallocate slice for
 * @peer_info:		destination peer
 *
 * If allocated, deallocate a slice for the given peer and discharge the
 * associated user quota. The peer_info lock must be held.
 */
void bus1_message_deallocate_locked(struct bus1_message *message,
				    struct bus1_peer_info *peer_info)
{
	lockdep_assert_held(&peer_info->lock);

	if (message->slice) {
		bus1_user_quota_discharge(peer_info, message->user,
					  message->data.n_bytes,
					  message->data.n_handles,
					  message->data.n_fds);
		message->slice = bus1_pool_release_kernel(&peer_info->pool,
							  message->slice);
	}

	message->user = bus1_user_unref(message->user);
}

/**
 * bus1_message_install_handles() - write handle ids to destination pool
 * @message:		message carrying handles
 * @peer_info:		destination peer
 *
 * Write out the handle ids of the handles carried in @message into the
 * destination slice.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_message_install_handles(struct bus1_message *message,
				 struct bus1_peer_info *peer_info)
{
	size_t pos, n, offset;
	struct kvec vec;
	u64 *ids;
	int r;

	offset = ALIGN(message->data.n_bytes, 8);
	pos = 0;
	ids = NULL;

	while ((n = bus1_handle_inflight_walk(&message->handles,
					      &pos, &ids)) > 0) {
		vec.iov_base = ids;
		vec.iov_len = n * sizeof(u64);
		r = bus1_pool_write_kvec(&peer_info->pool, message->slice,
					 offset, &vec, 1, vec.iov_len);
		if (r < 0)
			return r;

		offset += n * sizeof(u64);
	}

	return 0;
}

/**
 * bus1_message_install_fds() - install fds into destination peer
 * @message:		message carrying fds
 * @peer_info:		destination peer
 *
 * Install all the fds carried in @message into the destination peer, and write
 * out the fd numbers into the corresponding slice.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_message_install_fds(struct bus1_message *message,
			  struct bus1_peer_info *peer_info)
{
	size_t i, offset;
	struct kvec vec;
	int r, *fds;

	fds = kmalloc(message->data.n_fds * sizeof(*fds), GFP_TEMPORARY);
	if (!fds)
		return -ENOMEM;

	for (i = 0; i < message->data.n_fds; ++i) {
		r = get_unused_fd_flags(O_CLOEXEC);
		if (r < 0) {
			while (i--)
				put_unused_fd(fds[i]);
			kfree(fds);
			return r;
		}
		fds[i] = r;
	}

	vec.iov_base = fds;
	vec.iov_len = message->data.n_fds * sizeof(int);
	offset = ALIGN(message->data.n_bytes, 8) +
		 ALIGN(message->data.n_handles * sizeof(u64), 8);

	r = bus1_pool_write_kvec(&peer_info->pool, message->slice,
				 offset, &vec, 1, vec.iov_len);
	if (r < 0)
		goto error;

	for (i = 0; i < message->data.n_fds; ++i)
		fd_install(fds[i], get_file(message->files[i]));

	kfree(fds);
	return 0;

error:
	for (i = 0; i < message->data.n_fds; ++i)
		put_unused_fd(fds[i]);
	kfree(fds);
	return r;
}
