/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "memtx_space.h"
#include "space.h"
#include "iproto_constants.h"
#include "txn.h"
#include "tuple_compare.h"
#include "xrow.h"
#include "memtx_hash.h"
#include "memtx_tree.h"
#include "memtx_rtree.h"
#include "memtx_bitset.h"
#include "port.h"
#include "memtx_tuple.h"
#include "column_mask.h"
#include "sequence.h"

static void
memtx_space_destroy(struct space *space)
{
	free(space);
}

static size_t
memtx_space_bsize(struct space *space)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	return memtx_space->bsize;
}

/* {{{ DML */

void
memtx_space_update_bsize(struct space *space,
			 const struct tuple *old_tuple,
			 const struct tuple *new_tuple)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	ssize_t old_bsize = old_tuple ? box_tuple_bsize(old_tuple) : 0;
	ssize_t new_bsize = new_tuple ? box_tuple_bsize(new_tuple) : 0;
	assert((ssize_t)memtx_space->bsize + new_bsize - old_bsize >= 0);
	memtx_space->bsize += new_bsize - old_bsize;
}

/**
 * A version of space_replace for a space which has
 * no indexes (is not yet fully built).
 */
void
memtx_space_replace_no_keys(struct space *space, struct txn_stmt *,
			    enum dup_replace_mode)
{
	struct index *index = index_find_xc(space, 0);
	assert(index == NULL); /* not reached. */
	(void) index;
}

enum {
	/**
	 * This number is calculated based on the
	 * max (realistic) number of insertions
	 * a deletion from a B-tree or an R-tree
	 * can lead to, and, as a result, the max
	 * number of new block allocations.
	 */
	RESERVE_EXTENTS_BEFORE_DELETE = 8,
	RESERVE_EXTENTS_BEFORE_REPLACE = 16
};

/**
 * A short-cut version of replace() used during bulk load
 * from snapshot.
 */
void
memtx_space_replace_build_next(struct space *space, struct txn_stmt *stmt,
			       enum dup_replace_mode mode)
{
	assert(stmt->old_tuple == NULL && mode == DUP_INSERT);
	(void) mode;
	if (stmt->old_tuple) {
		/*
		 * Called from txn_rollback() In practice
		 * is impossible: all possible checks for tuple
		 * validity are done before the space is changed,
		 * and WAL is off, so this part can't fail.
		 */
		panic("Failed to commit transaction when loading "
		      "from snapshot");
	}
	index_build_next_xc(space->index[0], stmt->new_tuple);
	stmt->engine_savepoint = stmt;
	memtx_space_update_bsize(space, NULL, stmt->new_tuple);
}

/**
 * A short-cut version of replace() used when loading
 * data from XLOG files.
 */
void
memtx_space_replace_primary_key(struct space *space, struct txn_stmt *stmt,
				enum dup_replace_mode mode)
{
	stmt->old_tuple = index_replace_xc(space->index[0], stmt->old_tuple,
					   stmt->new_tuple, mode);
	stmt->engine_savepoint = stmt;
	memtx_space_update_bsize(space, stmt->old_tuple, stmt->new_tuple);
}

/**
 * @brief A single method to handle REPLACE, DELETE and UPDATE.
 *
 * @param space space
 * @param old_tuple the tuple that should be removed (can be NULL)
 * @param new_tuple the tuple that should be inserted (can be NULL)
 * @param mode      dup_replace_mode, used only if new_tuple is not
 *                  NULL and old_tuple is NULL, and only for the
 *                  primary key.
 *
 * For DELETE, new_tuple must be NULL. old_tuple must be
 * previously found in the primary key.
 *
 * For REPLACE, old_tuple must be NULL. The additional
 * argument dup_replace_mode further defines how REPLACE
 * should proceed.
 *
 * For UPDATE, both old_tuple and new_tuple must be given,
 * where old_tuple must be previously found in the primary key.
 *
 * Let's consider these three cases in detail:
 *
 * 1. DELETE, old_tuple is not NULL, new_tuple is NULL
 *    The effect is that old_tuple is removed from all
 *    indexes. dup_replace_mode is ignored.
 *
 * 2. REPLACE, old_tuple is NULL, new_tuple is not NULL,
 *    has one simple sub-case and two with further
 *    ramifications:
 *
 *	A. dup_replace_mode is DUP_INSERT. Attempts to insert the
 *	new tuple into all indexes. If *any* of the unique indexes
 *	has a duplicate key, deletion is aborted, all of its
 *	effects are removed, and an error is thrown.
 *
 *	B. dup_replace_mode is DUP_REPLACE. It means an existing
 *	tuple has to be replaced with the new one. To do it, tries
 *	to find a tuple with a duplicate key in the primary index.
 *	If the tuple is not found, throws an error. Otherwise,
 *	replaces the old tuple with a new one in the primary key.
 *	Continues on to secondary keys, but if there is any
 *	secondary key, which has a duplicate tuple, but one which
 *	is different from the duplicate found in the primary key,
 *	aborts, puts everything back, throws an exception.
 *
 *	For example, if there is a space with 3 unique keys and
 *	two tuples { 1, 2, 3 } and { 3, 1, 2 }:
 *
 *	This REPLACE/DUP_REPLACE is OK: { 1, 5, 5 }
 *	This REPLACE/DUP_REPLACE is not OK: { 2, 2, 2 } (there
 *	is no tuple with key '2' in the primary key)
 *	This REPLACE/DUP_REPLACE is not OK: { 1, 1, 1 } (there
 *	is a conflicting tuple in the secondary unique key).
 *
 *	C. dup_replace_mode is DUP_REPLACE_OR_INSERT. If
 *	there is a duplicate tuple in the primary key, behaves the
 *	same way as DUP_REPLACE, otherwise behaves the same way as
 *	DUP_INSERT.
 *
 * 3. UPDATE has to delete the old tuple and insert a new one.
 *    dup_replace_mode is ignored.
 *    Note that old_tuple primary key doesn't have to match
 *    new_tuple primary key, thus a duplicate can be found.
 *    For this reason, and since there can be duplicates in
 *    other indexes, UPDATE is the same as DELETE +
 *    REPLACE/DUP_INSERT.
 *
 * @return old_tuple. DELETE, UPDATE and REPLACE/DUP_REPLACE
 * always produce an old tuple. REPLACE/DUP_INSERT always returns
 * NULL. REPLACE/DUP_REPLACE_OR_INSERT may or may not find
 * a duplicate.
 *
 * The method is all-or-nothing in all cases. Changes are either
 * applied to all indexes, or nothing applied at all.
 *
 * Note, that even in case of REPLACE, dup_replace_mode only
 * affects the primary key, for secondary keys it's always
 * DUP_INSERT.
 *
 * The call never removes more than one tuple: if
 * old_tuple is given, dup_replace_mode is ignored.
 * Otherwise, it's taken into account only for the
 * primary key.
 */
void
memtx_space_replace_all_keys(struct space *space, struct txn_stmt *stmt,
			     enum dup_replace_mode mode)
{
	struct tuple *old_tuple = stmt->old_tuple;
	struct tuple *new_tuple = stmt->new_tuple;
	/*
	 * Ensure we have enough slack memory to guarantee
	 * successful statement-level rollback.
	 */
	memtx_index_extent_reserve(new_tuple ?
				   RESERVE_EXTENTS_BEFORE_REPLACE :
				   RESERVE_EXTENTS_BEFORE_DELETE);
	uint32_t i = 0;
	try {
		/* Update the primary key */
		struct index *pk = index_find_xc(space, 0);
		assert(pk->def->opts.is_unique);
		/*
		 * If old_tuple is not NULL, the index
		 * has to find and delete it, or raise an
		 * error.
		 */
		old_tuple = index_replace_xc(pk, old_tuple, new_tuple, mode);

		assert(old_tuple || new_tuple);
		/* Update secondary keys. */
		for (i++; i < space->index_count; i++) {
			struct index *index = space->index[i];
			index_replace_xc(index, old_tuple, new_tuple, DUP_INSERT);
		}
	} catch (Exception *e) {
		/* Rollback all changes */
		for (; i > 0; i--) {
			struct index *index = space->index[i-1];
			index_replace_xc(index, new_tuple, old_tuple, DUP_INSERT);
		}
		throw;
	}
	stmt->old_tuple = old_tuple;
	stmt->engine_savepoint = stmt;
	memtx_space_update_bsize(space, old_tuple, new_tuple);
}

static inline enum dup_replace_mode
dup_replace_mode(uint32_t op)
{
	return op == IPROTO_INSERT ? DUP_INSERT : DUP_REPLACE_OR_INSERT;
}

static void
memtx_space_apply_initial_join_row(struct space *space, struct request *request)
{
	if (request->type != IPROTO_INSERT) {
		tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
				(uint32_t) request->type);
	}
	request->header->replica_id = 0;
	struct txn *txn = txn_begin_stmt(space);
	try {
		space_execute_replace_xc(space, txn, request);
		txn_commit_stmt(txn, request);
	} catch (Exception *e) {
		say_error("rollback: %s", e->errmsg);
		txn_rollback_stmt();
		throw;
	}
	/** The new tuple is referenced by the primary key. */
}

static struct tuple *
memtx_space_execute_replace(struct space *space, struct txn *txn,
			    struct request *request)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	enum dup_replace_mode mode = dup_replace_mode(request->type);
	stmt->new_tuple = memtx_tuple_new_xc(space->format, request->tuple,
					     request->tuple_end);
	tuple_ref(stmt->new_tuple);
	memtx_space->replace(space, stmt, mode);
	/** The new tuple is referenced by the primary key. */
	return stmt->new_tuple;
}

static struct tuple *
memtx_space_execute_delete(struct space *space, struct txn *txn,
			   struct request *request)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	/* Try to find the tuple by unique key. */
	struct index *pk = index_find_unique(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	if (exact_key_validate(pk->def->key_def, key, part_count) != 0)
		diag_raise();
	stmt->old_tuple = index_get_xc(pk, key, part_count);
	if (stmt->old_tuple)
		memtx_space->replace(space, stmt, DUP_REPLACE_OR_INSERT);
	return stmt->old_tuple;
}

static struct tuple *
memtx_space_execute_update(struct space *space, struct txn *txn,
			   struct request *request)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	/* Try to find the tuple by unique key. */
	struct index *pk = index_find_unique(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	if (exact_key_validate(pk->def->key_def, key, part_count) != 0)
		diag_raise();
	stmt->old_tuple = index_get_xc(pk, key, part_count);

	if (stmt->old_tuple == NULL)
		return NULL;

	/* Update the tuple; legacy, request ops are in request->tuple */
	uint32_t new_size = 0, bsize;
	const char *old_data = tuple_data_range(stmt->old_tuple, &bsize);
	const char *new_data =
		tuple_update_execute(region_aligned_alloc_cb, &fiber()->gc,
				     request->tuple, request->tuple_end,
				     old_data, old_data + bsize,
				     &new_size, request->index_base, NULL);
	if (new_data == NULL)
		diag_raise();

	stmt->new_tuple = memtx_tuple_new_xc(space->format, new_data,
					     new_data + new_size);
	tuple_ref(stmt->new_tuple);
	if (stmt->old_tuple)
		memtx_space->replace(space, stmt, DUP_REPLACE);
	return stmt->new_tuple;
}

static void
memtx_space_execute_upsert(struct space *space, struct txn *txn,
			   struct request *request)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	/*
	 * Check all tuple fields: we should produce an error on
	 * malformed tuple even if upsert turns into an update.
	 */
	if (tuple_validate_raw(space->format, request->tuple))
		diag_raise();

	struct index *index = index_find_unique(space, 0);

	uint32_t part_count = index->def->key_def->part_count;
	/* Extract the primary key from tuple. */
	const char *key = tuple_extract_key_raw(request->tuple,
						request->tuple_end,
						index->def->key_def, NULL);
	if (key == NULL)
		diag_raise();
	/* Cut array header */
	mp_decode_array(&key);

	/* Try to find the tuple by primary key. */
	stmt->old_tuple = index_get_xc(index, key, part_count);

	if (stmt->old_tuple == NULL) {
		/**
		 * Old tuple was not found. A write optimized
		 * engine may only know this after commit, so
		 * some errors which happen on this branch would
		 * only make it to the error log in it.
		 * To provide identical semantics, we should not throw
		 * anything. However, considering the kind of
		 * error which may occur, throwing it won't break
		 * cross-engine compatibility:
		 * - update ops are checked before commit
		 * - OOM may happen at any time
		 * - duplicate key has to be checked by
		 *   write-optimized engine before commit, so if
		 *   we get it here, it's also OK to throw it
		 * @sa https://github.com/tarantool/tarantool/issues/1156
		 */
		if (tuple_update_check_ops(region_aligned_alloc_xc_cb, &fiber()->gc,
				       request->ops, request->ops_end,
				       request->index_base)) {
			diag_raise();
		}
		stmt->new_tuple = memtx_tuple_new_xc(space->format,
						     request->tuple,
						     request->tuple_end);
		tuple_ref(stmt->new_tuple);
	} else {
		uint32_t new_size = 0, bsize;
		const char *old_data = tuple_data_range(stmt->old_tuple,
							&bsize);
		/*
		 * Update the tuple.
		 * tuple_upsert_execute() fails on totally wrong
		 * tuple ops, but ignores ops that not suitable
		 * for the tuple.
		 */
		uint64_t column_mask = COLUMN_MASK_FULL;
		const char *new_data =
			tuple_upsert_execute(region_aligned_alloc_cb,
					     &fiber()->gc, request->ops,
					     request->ops_end, old_data,
					     old_data + bsize, &new_size,
					     request->index_base, false,
					     &column_mask);
		if (new_data == NULL)
			diag_raise();

		stmt->new_tuple = memtx_tuple_new_xc(space->format, new_data,
						     new_data + new_size);
		tuple_ref(stmt->new_tuple);

		struct index *pk = space->index[0];
		if (!key_update_can_be_skipped(pk->def->key_def->column_mask,
					       column_mask) &&
		    tuple_compare(stmt->old_tuple, stmt->new_tuple,
				  pk->def->key_def) != 0) {
			/* Primary key is changed: log error and do nothing. */
			diag_set(ClientError, ER_CANT_UPDATE_PRIMARY_KEY,
				 pk->def->name, space_name(space));
			diag_log();
			tuple_unref(stmt->new_tuple);
			stmt->old_tuple = NULL;
			stmt->new_tuple = NULL;
		}
	}
	/*
	 * It's OK to use DUP_REPLACE_OR_INSERT: we don't risk
	 * inserting a new tuple if the old one exists, since
	 * we checked this case explicitly and skipped the upsert
	 * above.
	 */
	if (stmt->new_tuple)
		memtx_space->replace(space, stmt, DUP_REPLACE_OR_INSERT);
	/* Return nothing: UPSERT does not return data. */
}

static void
memtx_space_execute_select(struct space *space, struct txn *txn,
			   uint32_t index_id, uint32_t iterator,
			   uint32_t offset, uint32_t limit,
			   const char *key, const char *key_end,
			   struct port *port)
{
	(void)txn;
	(void)key_end;

	struct index *index = index_find_xc(space, index_id);

	ERROR_INJECT_EXCEPTION(ERRINJ_TESTING);

	uint32_t found = 0;
	if (iterator >= iterator_type_MAX)
		tnt_raise(IllegalParams, "Invalid iterator type");
	enum iterator_type type = (enum iterator_type) iterator;

	uint32_t part_count = key ? mp_decode_array(&key) : 0;
	if (key_validate(index->def, type, key, part_count))
		diag_raise();

	struct iterator *it = index_position_xc(index);
	index_init_iterator_xc(index, it, type, key, part_count);

	struct tuple *tuple;
	while ((tuple = iterator_next_xc(it)) != NULL) {
		if (offset > 0) {
			offset--;
			continue;
		}
		if (limit == found++)
			break;
		port_add_tuple_xc(port, tuple);
	}
}

/* }}} DML */

/* {{{ DDL */

static void
memtx_space_check_index_def(struct space *space, struct index_def *index_def)
{
	if (index_def->key_def->is_nullable) {
		if (index_def->iid == 0) {
			tnt_raise(ClientError, ER_NULLABLE_PRIMARY,
				  space_name(space));
		}
		if (index_def->type != TREE) {
			tnt_raise(ClientError, ER_UNSUPPORTED,
				  index_type_strs[index_def->type],
				  "nullable parts");
		}
	}
	switch (index_def->type) {
	case HASH:
		if (! index_def->opts.is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "HASH index must be unique");
		}
		break;
	case TREE:
		/* TREE index has no limitations. */
		break;
	case RTREE:
		if (index_def->key_def->part_count != 1) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "RTREE index key can not be multipart");
		}
		if (index_def->opts.is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "RTREE index can not be unique");
		}
		if (index_def->key_def->parts[0].type != FIELD_TYPE_ARRAY) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "RTREE index field type must be ARRAY");
		}
		/* no furter checks of parts needed */
		return;
	case BITSET:
		if (index_def->key_def->part_count != 1) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "BITSET index key can not be multipart");
		}
		if (index_def->opts.is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "BITSET can not be unique");
		}
		if (index_def->key_def->parts[0].type != FIELD_TYPE_UNSIGNED &&
		    index_def->key_def->parts[0].type != FIELD_TYPE_STRING) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  "BITSET index field type must be NUM or STR");
		}
		/* no furter checks of parts needed */
		return;
	default:
		tnt_raise(ClientError, ER_INDEX_TYPE,
			  index_def->name,
			  space_name(space));
		break;
	}
	/* Only HASH and TREE indexes checks parts there */
	/* Check that there are no ANY, ARRAY, MAP parts */
	for (uint32_t i = 0; i < index_def->key_def->part_count; i++) {
		struct key_part *part = &index_def->key_def->parts[i];
		if (part->type <= FIELD_TYPE_ANY ||
		    part->type >= FIELD_TYPE_ARRAY) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  index_def->name,
				  space_name(space),
				  tt_sprintf("field type '%s' is not supported",
					     field_type_strs[part->type]));
		}
	}
}

static struct snapshot_iterator *
sequence_data_index_create_snapshot_iterator(struct index *)
{
	return sequence_data_iterator_create();
}

static struct index *
sequence_data_index_new(struct index_def *def)
{
	struct memtx_hash_index *index = memtx_hash_index_new(def);
	if (index == NULL)
		return NULL;
	static struct index_vtab vtab = *index->base.vtab;
	vtab.create_snapshot_iterator =
		sequence_data_index_create_snapshot_iterator;
	index->base.vtab = &vtab;
	return &index->base;
}

static struct index *
memtx_space_create_index(struct space *space, struct index_def *index_def)
{
	struct index *index;

	if (space->def->id == BOX_SEQUENCE_DATA_ID) {
		/*
		 * The content of _sequence_data is not updated
		 * when a sequence is used for auto increment in
		 * a space. To make sure all sequence values are
		 * written to snapshot, use a special snapshot
		 * iterator that walks over the sequence cache.
		 */
		index = sequence_data_index_new(index_def);
		if (index == NULL)
			diag_raise();
		return index;
	}

	switch (index_def->type) {
	case HASH:
		index = (struct index *)memtx_hash_index_new(index_def);
		break;
	case TREE:
		index = (struct index *)memtx_tree_index_new(index_def);
		break;
	case RTREE:
		index = (struct index *)memtx_rtree_index_new(index_def);
		break;
	case BITSET:
		index = (struct index *)memtx_bitset_index_new(index_def);
		break;
	default:
		unreachable();
		return NULL;
	}
	if (index == NULL)
		diag_raise();
	return index;
}

/**
 * Replicate engine state in a newly created space.
 * This function is invoked when executing a replace into _index
 * space originating either from a snapshot or from the binary
 * log. It brings the newly created space up to date with the
 * engine recovery state: if the event comes from the snapshot,
 * then the primary key is not built, otherwise it's created
 * right away.
 */
static void
memtx_space_do_add_primary_key(struct space *space,
			       enum memtx_recovery_state state)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	switch (state) {
	case MEMTX_INITIALIZED:
		panic("can't create a new space before snapshot recovery");
		break;
	case MEMTX_INITIAL_RECOVERY:
		index_begin_build(space->index[0]);
		memtx_space->replace = memtx_space_replace_build_next;
		break;
	case MEMTX_FINAL_RECOVERY:
		index_begin_build(space->index[0]);
		index_end_build(space->index[0]);
		memtx_space->replace = memtx_space_replace_primary_key;
		break;
	case MEMTX_OK:
		index_begin_build(space->index[0]);
		index_end_build(space->index[0]);
		memtx_space->replace = memtx_space_replace_all_keys;
		break;
	}
}

static void
memtx_space_add_primary_key(struct space *space)
{
	memtx_space_do_add_primary_key(space,
		((MemtxEngine *)space->engine)->m_state);
}

static void
memtx_space_check_format(struct space *new_space, struct space *old_space)
{
	if (old_space->index_count == 0 ||
	    index_size(old_space->index[0]) == 0)
		return;
	struct index *pk = index_find_xc(old_space, 0);
	struct iterator *it = index_alloc_iterator_xc(pk);
	IteratorGuard guard(it);
	index_init_iterator_xc(pk, it, ITER_ALL, NULL, 0);

	struct tuple *tuple;
	while ((tuple = iterator_next_xc(it)) != NULL) {
		/*
		 * Check that the tuple is OK according to the
		 * new format.
		 */
		if (tuple_validate(new_space->format, tuple))
			diag_raise();
	}
}

static void
memtx_space_drop_primary_key(struct space *space)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	memtx_space->replace = memtx_space_replace_no_keys;
}

static void
memtx_init_system_space(struct space *space)
{
	memtx_space_do_add_primary_key(space, MEMTX_OK);
}

static void
memtx_space_build_secondary_key(struct space *old_space,
				struct space *new_space,
				struct index *new_index)
{
	/**
	 * If it's a secondary key, and we're not building them
	 * yet (i.e. it's snapshot recovery for memtx), do nothing.
	 */
	if (new_index->def->iid != 0) {
		struct memtx_space *memtx_space;
		memtx_space = (struct memtx_space *)new_space;
		if (!(memtx_space->replace == memtx_space_replace_all_keys))
			return;
	}
	struct index *pk = index_find_xc(old_space, 0);

	struct errinj *inj = errinj(ERRINJ_BUILD_SECONDARY, ERRINJ_INT);
	if (inj != NULL && inj->iparam == (int)new_index->def->iid) {
		tnt_raise(ClientError, ER_INJECTION, "buildSecondaryKey");
	}

	/* Now deal with any kind of add index during normal operation. */
	struct iterator *it = index_alloc_iterator_xc(pk);
	IteratorGuard guard(it);
	index_init_iterator_xc(pk, it, ITER_ALL, NULL, 0);

	/*
	 * The index has to be built tuple by tuple, since
	 * there is no guarantee that all tuples satisfy
	 * new index' constraints. If any tuple can not be
	 * added to the index (insufficient number of fields,
	 * etc., the build is aborted.
	 */
	/* Build the new index. */
	struct tuple *tuple;
	while ((tuple = iterator_next_xc(it)) != NULL) {
		/*
		 * Check that the tuple is OK according to the
		 * new format.
		 */
		if (tuple_validate(new_space->format, tuple))
			diag_raise();
		/*
		 * @todo: better message if there is a duplicate.
		 */
		struct tuple *old_tuple = index_replace_xc(new_index, NULL,
							   tuple, DUP_INSERT);
		assert(old_tuple == NULL); /* Guaranteed by DUP_INSERT. */
		(void) old_tuple;
	}
}

static void
memtx_space_prepare_truncate(struct space *old_space,
			     struct space *new_space)
{
	struct memtx_space *old_memtx_space = (struct memtx_space *)old_space;
	struct memtx_space *new_memtx_space = (struct memtx_space *)new_space;
	new_memtx_space->replace = old_memtx_space->replace;
}

static void
memtx_space_prune(struct space *space)
{
	struct index *index = space_index(space, 0);
	if (index == NULL)
		return;

	struct iterator *it = index_position_xc(index);
	index_init_iterator_xc(index, it, ITER_ALL, NULL, 0);
	struct tuple *tuple;
	while ((tuple = iterator_next_xc(it)) != NULL)
		tuple_unref(tuple);
}

static void
memtx_space_commit_truncate(struct space *old_space,
			    struct space *new_space)
{
	(void)new_space;
	memtx_space_prune(old_space);
}

static void
memtx_space_prepare_alter(struct space *old_space, struct space *new_space)
{
	struct memtx_space *old_memtx_space = (struct memtx_space *)old_space;
	struct memtx_space *new_memtx_space = (struct memtx_space *)new_space;
	new_memtx_space->replace = old_memtx_space->replace;
	bool is_empty = old_space->index_count == 0 ||
			index_size(old_space->index[0]) == 0;
	space_def_check_compatibility_xc(old_space->def, new_space->def,
					 is_empty);
}

static void
memtx_space_commit_alter(struct space *old_space, struct space *new_space)
{
	struct memtx_space *old_memtx_space = (struct memtx_space *)old_space;
	struct memtx_space *new_memtx_space = (struct memtx_space *)new_space;

	/* Delete all tuples when the last index is dropped. */
	if (new_space->index_count == 0)
		memtx_space_prune(old_space);
	else
		new_memtx_space->bsize = old_memtx_space->bsize;
}

/* }}} DDL */

const struct space_vtab memtx_space_vtab = {
	/* .destroy = */ memtx_space_destroy,
	/* .bsize = */ memtx_space_bsize,
	/* .apply_initial_join_row = */ memtx_space_apply_initial_join_row,
	/* .execute_replace = */ memtx_space_execute_replace,
	/* .execute_delete = */ memtx_space_execute_delete,
	/* .execute_update = */ memtx_space_execute_update,
	/* .execute_upsert = */ memtx_space_execute_upsert,
	/* .execute_select = */ memtx_space_execute_select,
	/* .init_system_space = */ memtx_init_system_space,
	/* .check_index_def = */ memtx_space_check_index_def,
	/* .create_index = */ memtx_space_create_index,
	/* .add_primary_key = */ memtx_space_add_primary_key,
	/* .drop_primary_key = */ memtx_space_drop_primary_key,
	/* .check_format  = */ memtx_space_check_format,
	/* .build_secondary_key = */ memtx_space_build_secondary_key,
	/* .prepare_truncate = */ memtx_space_prepare_truncate,
	/* .commit_truncate = */ memtx_space_commit_truncate,
	/* .prepare_alter = */ memtx_space_prepare_alter,
	/* .commit_alter = */ memtx_space_commit_alter,
};
