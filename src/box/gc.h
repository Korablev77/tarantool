#ifndef TARANTOOL_BOX_GC_H_INCLUDED
#define TARANTOOL_BOX_GC_H_INCLUDED
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stddef.h>

#include "vclock.h"
#include "latch.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct gc_consumer;

enum { GC_NAME_MAX = 64 };

/** Consumer type: WAL consumer, or SNAP */
enum gc_consumer_type {
	GC_CONSUMER_WAL = 1,
	GC_CONSUMER_SNAP = 2,
	GC_CONSUMER_ALL = 3,
};

typedef rb_node(struct gc_consumer) gc_node_t;

/**
 * The object of this type is used to prevent garbage
 * collection from removing files that are still in use.
 */
struct gc_consumer {
	/** Link in gc_state::consumers. */
	gc_node_t node;
	/** Human-readable name. */
	char name[GC_NAME_MAX];
	/** The vclock tracked by this consumer. */
	struct vclock vclock;
	/** Consumer type, indicating that consumer only consumes
	 * WAL files, or both - SNAP and WAL.
	 */
	enum gc_consumer_type type;
};

typedef rb_tree(struct gc_consumer) gc_tree_t;

/** Garbage collection state. */
struct gc_state {
	/** Number of checkpoints to maintain. */
	int checkpoint_count;
	/** Max vclock WAL garbage collection has been called for. */
	struct vclock wal_vclock;
	/** Max vclock checkpoint garbage collection has been called for. */
	struct vclock checkpoint_vclock;
	/** Registered consumers, linked by gc_consumer::node. */
	gc_tree_t consumers;
	/**
	 * Latch serializing concurrent invocations of engine
	 * garbage collection callbacks.
	 */
	struct latch latch;
};
extern struct gc_state gc;

/**
 * Initialize the garbage collection state.
 */
void
gc_init(void);

/**
 * Destroy the garbage collection state.
 */
void
gc_free(void);

/**
 * Invoke garbage collection in order to remove files left
 * from old checkpoints. The number of checkpoints saved by
 * this function is specified by box.cfg.checkpoint_count.
 */
void
gc_run(void);

/**
 * Update the checkpoint_count configuration option and
 * rerun garbage collection.
 */
void
gc_set_checkpoint_count(int checkpoint_count);

/**
 * Register a consumer.
 *
 * This will stop garbage collection of objects newer than
 * @vclock until the consumer is unregistered or advanced.
 * @name is a human-readable name of the consumer, it will
 * be used for reporting the consumer to the user.
 * @type consumer type, reporting whether consumer only depends
 * on WAL files.
 *
 * Returns a pointer to the new consumer object or NULL on
 * memory allocation failure.
 */
struct gc_consumer *
gc_consumer_register(const char *name, const struct vclock *vclock,
		     enum gc_consumer_type type);

/**
 * Unregister a consumer and invoke garbage collection
 * if needed.
 */
void
gc_consumer_unregister(struct gc_consumer *consumer);

/**
 * Advance the vclock tracked by a consumer and
 * invoke garbage collection if needed.
 */
void
gc_consumer_advance(struct gc_consumer *consumer, const struct vclock *vclock);

/**
 * Iterator over registered consumers. The iterator is valid
 * as long as the caller doesn't yield.
 */
struct gc_consumer_iterator {
	struct gc_consumer *curr;
};

/** Init an iterator over consumers. */
static inline void
gc_consumer_iterator_init(struct gc_consumer_iterator *it)
{
	it->curr = NULL;
}

/**
 * Iterate to the next registered consumer. Return a pointer
 * to the next consumer object or NULL if there is no more
 * consumers.
 */
struct gc_consumer *
gc_consumer_iterator_next(struct gc_consumer_iterator *it);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_GC_H_INCLUDED */
