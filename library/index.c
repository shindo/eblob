/*
 * 2011+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This file is part of Eblob.
 * 
 * Eblob is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Eblob is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Eblob.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Each base has index represented by continuous array of disk control
 * structures.
 * Each "closed" base has sorted on-disk index for logarithmic search via
 * bsearch(3)
 *
 * Index consists of blocks to narrow down binary search, on top of blocks
 * there is bloom filter to speed up rather expensive search of non-existent
 * entries.
 */

#include "features.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blob.h"

#include "react/eblob_react.h"

#include <handystats/measuring_points.h>


int eblob_key_sort(const void *key1, const void *key2)
{
	return eblob_id_cmp(((struct eblob_key *)key1)->id, ((struct eblob_key *)key2)->id);
}

int eblob_disk_control_sort(const void *d1, const void *d2)
{
	const struct eblob_disk_control *dc1 = d1;
	const struct eblob_disk_control *dc2 = d2;

	return eblob_id_cmp(dc1->key.id, dc2->key.id);
}

int eblob_disk_control_sort_with_flags(const void *d1, const void *d2)
{
	const struct eblob_disk_control *dc1 = d1;
	const struct eblob_disk_control *dc2 = d2;

	int cmp = eblob_id_cmp(dc1->key.id, dc2->key.id);

	if (cmp == 0) {
		if ((dc1->flags & BLOB_DISK_CTL_REMOVE) && !(dc2->flags & BLOB_DISK_CTL_REMOVE))
			cmp = -1;
		
		if (!(dc1->flags & BLOB_DISK_CTL_REMOVE) && (dc2->flags & BLOB_DISK_CTL_REMOVE))
			cmp = 1;
	}

	return cmp;
}

static int eblob_key_range_cmp(const void *k1, const void *k2)
{
	const struct eblob_key *key = k1;
	const struct eblob_index_block *index = k2;
	int cmp;

	/* compare key against start of the [start_key, end_key] range */
	cmp = eblob_id_cmp(key->id, index->start_key.id);

	/* our key is less than the start, skip */
	if (cmp < 0)
		return -1;

	/* our key belongs to the range - it is equal to the start of the range - accept */
	if (cmp == 0)
		return 0;

	/* compare key against end of the [start_key, end_key] range
	 * our key is already bigger than start of the range
	 */
	cmp = eblob_id_cmp(key->id, index->end_key.id);

	/* our key is less or equal than the end of the range - accept */
	if (cmp < 0)
		return 0;
	if (cmp == 0)
		return 0;

	/* key is bigger than the end of the range - skip */
	return 1;
}

int eblob_index_block_cmp(const void *k1, const void *k2)
{
	const struct eblob_index_block *k = k1;
	return eblob_key_range_cmp(&k->start_key, k2);
}

static int eblob_find_non_removed_callback(struct eblob_disk_control *sorted,
		struct eblob_disk_control *dc __attribute_unused__)
{
	uint64_t rem = eblob_bswap64(BLOB_DISK_CTL_REMOVE);
	return !(sorted->flags & rem);
}

int eblob_index_blocks_destroy(struct eblob_base_ctl *bctl)
{
	pthread_rwlock_wrlock(&bctl->index_blocks_lock);
	/* Free data */
	free(bctl->index_blocks);
	free(bctl->bloom);
	/* Allow subsequent destroys */
	bctl->index_blocks = NULL;
	bctl->bloom = NULL;
	/* Nullify stats */
	eblob_stat_set(bctl->stat, EBLOB_LST_BLOOM_SIZE, 0);
	eblob_stat_set(bctl->stat, EBLOB_LST_INDEX_BLOCKS_SIZE, 0);
	pthread_rwlock_unlock(&bctl->index_blocks_lock);

	return 0;
}

struct eblob_index_block *eblob_index_blocks_search_nolock_bsearch_nobloom(struct eblob_base_ctl *bctl, struct eblob_disk_control *dc,
		struct eblob_disk_search_stat *st)
{
	react_start_action(ACTION_EBLOB_INDEX_BLOCK_SEARCH_NOLOCK_BSEARCH_NOBLOOM);
	struct eblob_index_block *t = NULL;

	/*
	 * Use binary search to find given eblob_index_block in bctl->index_blocks
	 * Blocks were placed into that array in sorted order.
	 */
	t = bsearch(&dc->key, bctl->index_blocks,
		eblob_stat_get(bctl->stat, EBLOB_LST_INDEX_BLOCKS_SIZE) / sizeof(struct eblob_index_block),
		sizeof(struct eblob_index_block), eblob_key_range_cmp);
	if (t)
		st->found_index_block++;

	react_stop_action(ACTION_EBLOB_INDEX_BLOCK_SEARCH_NOLOCK_BSEARCH_NOBLOOM);
	return t;
}

struct eblob_index_block *eblob_index_blocks_search_nolock(struct eblob_base_ctl *bctl, struct eblob_disk_control *dc,
		struct eblob_disk_search_stat *st)
{
	react_start_action(ACTION_EBLOB_INDEX_BLOCK_SEARCH_NOLOCK);

	struct eblob_index_block *t = NULL;

	if (!eblob_bloom_get(bctl, &dc->key)) {
		st->bloom_null++;
		react_stop_action(ACTION_EBLOB_INDEX_BLOCK_SEARCH_NOLOCK);
		return NULL;
	}

	t = eblob_index_blocks_search_nolock_bsearch_nobloom(bctl, dc, st);
	if (!t)
		st->no_block++;

	react_stop_action(ACTION_EBLOB_INDEX_BLOCK_SEARCH_NOLOCK);
	return t;
}

/*!
 * Calculate bloom filter size based on index file size
 */
static uint64_t eblob_bloom_size(const struct eblob_base_ctl *bctl)
{
	uint64_t bloom_size = 0;

	/* Number of record in base */
	bloom_size += bctl->sort.size / sizeof(struct eblob_disk_control);
	/* Number of index blocks in base */
	bloom_size /= bctl->back->cfg.index_block_size;
	/* Add one for tiny bases */
	bloom_size += 1;
	/* Number of bits in bloom for one block */
	bloom_size *= bctl->back->cfg.index_block_bloom_length;
	/* Size of byte */
	bloom_size /= 8;

	return bloom_size;
}

/*!
 * Calculates number of needed hash functions.
 * An optimal number of hash functions
 *	k = (m/n) \ln 2
 * has been assumed.
 *
 * It uses [1, 32] sanity boundary.
 */
static uint8_t eblob_bloom_func_num(const struct eblob_base_ctl *bctl)
{
	uint64_t bits_per_key;
	uint8_t func_num = 0;

	bits_per_key = 8 * bctl->bloom_size /
		(bctl->sort.size / sizeof(struct eblob_disk_control));
	func_num = bits_per_key * 0.69;
	if (func_num == 0)
		return 1;
	if (func_num > 20)
		return 20;
	return func_num;
}

int eblob_index_blocks_fill(struct eblob_base_ctl *bctl)
{
	struct eblob_index_block *block = NULL;
	struct eblob_disk_control dc;
	uint64_t block_count, block_id = 0, err_count = 0, offset = 0;
	int64_t removed = 0;
	int64_t removed_size = 0;
	unsigned int i;
	int err = 0;

	/* Allocate bloom filter */
	bctl->bloom_size = eblob_bloom_size(bctl);
	EBLOB_WARNX(bctl->back->cfg.log, EBLOB_LOG_NOTICE,
			"index: bloom filter size: %" PRIu64, bctl->bloom_size);

	/* Calculate needed number of hash functions */
	bctl->bloom_func_num = eblob_bloom_func_num(bctl);

	bctl->bloom = calloc(1, bctl->bloom_size);
	if (bctl->bloom == NULL) {
		err = -err;
		goto err_out_exit;
	}
	eblob_stat_set(bctl->stat, EBLOB_LST_BLOOM_SIZE, bctl->bloom_size);

	/* Pre-allcate all index blocks */
	block_count = howmany(bctl->sort.size / sizeof(struct eblob_disk_control),
			bctl->back->cfg.index_block_size);
	bctl->index_blocks = calloc(block_count, sizeof(struct eblob_index_block));
	if (bctl->index_blocks == NULL) {
		err = -ENOMEM;
		goto err_out_exit;
	}
	eblob_stat_set(bctl->stat, EBLOB_LST_INDEX_BLOCKS_SIZE,
			block_count * sizeof(struct eblob_index_block));

	while (offset < bctl->sort.size) {
		block = &bctl->index_blocks[block_id++];
		block->start_offset = offset;
		for (i = 0; i < bctl->back->cfg.index_block_size && offset < bctl->sort.size; ++i) {
			err = pread(bctl->sort.fd, &dc, sizeof(struct eblob_disk_control), offset);
			if (err != sizeof(struct eblob_disk_control)) {
				if (err < 0)
					err = -errno;
				goto err_out_drop_tree;
			}

			/* Check record for validity */
			err = eblob_check_record(bctl, &dc);
			if (err != 0) {
				/* Bump stats */
				eblob_stat_inc(bctl->stat, EBLOB_LST_INDEX_CORRUPTED_ENTRIES);

				/*
				 * We can't recover from broken first or last
				 * entry of index block.
				 */
				if (err_count++ > EBLOB_BLOB_INDEX_CORRUPT_MAX
						|| i == 0 || i == bctl->back->cfg.index_block_size - 1) {
					EBLOB_WARNC(bctl->back->cfg.log, EBLOB_LOG_ERROR, -err,
							"EB0001: too many index corruptions: %" PRIu64
							", can not continue", err_count);
					EBLOB_WARNX(bctl->back->cfg.log, EBLOB_LOG_ERROR,
							"running `eblob_merge` on '%s' should help:", bctl->name);
					EBLOB_WARNX(bctl->back->cfg.log, EBLOB_LOG_ERROR,
							"http://doc.reverbrain.com/kb:eblob:eb0001-index-corruption");
					goto err_out_drop_tree;
				}
				offset += sizeof(struct eblob_disk_control);
				continue;
			}

			if (i == 0)
				block->start_key = dc.key;

			if (dc.flags & eblob_bswap64(BLOB_DISK_CTL_REMOVE)) {
				removed++;
				removed_size += dc.disk_size;
			} else {
				eblob_bloom_set(bctl, &dc.key);
			}

			offset += sizeof(struct eblob_disk_control);
		}

		block->end_offset = offset;
		block->end_key = dc.key;
	}
	eblob_stat_set(bctl->stat, EBLOB_LST_RECORDS_REMOVED, removed);
	eblob_stat_set(bctl->stat, EBLOB_LST_REMOVED_SIZE, removed_size);
	return 0;

err_out_drop_tree:
	eblob_index_blocks_destroy(bctl);
err_out_exit:
	return err;
}


static struct eblob_disk_control *eblob_find_on_disk(struct eblob_backend *b,
		struct eblob_base_ctl *bctl, struct eblob_disk_control *dc,
		int (* callback)(struct eblob_disk_control *sorted, struct eblob_disk_control *dc),
		struct eblob_disk_search_stat *st)
{
	react_start_action(ACTION_EBLOB_FIND_ON_DISK);
	HANDY_TIMER_SCOPE("eblob.disk.index.lookup", pthread_self());

	struct eblob_disk_control *sorted, *end, *sorted_orig, *start, *found = NULL;
	struct eblob_disk_control *search_start, *search_end;
	struct eblob_index_block *block;
	size_t num;
	const int hdr_size = sizeof(struct eblob_disk_control);

	st->search_on_disk++;

	end = bctl->sort.data + bctl->sort.size;
	start = bctl->sort.data;

	pthread_rwlock_rdlock(&bctl->index_blocks_lock);
	block = eblob_index_blocks_search_nolock(bctl, dc, st);
	if (block) {
		assert((bctl->sort.size - block->start_offset) / hdr_size > 0);
		assert((bctl->sort.size - block->start_offset) % hdr_size == 0);

		num = (bctl->sort.size - block->start_offset) / hdr_size;

		if (num > b->cfg.index_block_size)
			num = b->cfg.index_block_size;

		search_start = bctl->sort.data + block->start_offset;
		/*
		 * We do not use @block->end_offset here, since it points to
		 * the start offset of the *next* record, which potentially
		 * can be outside of the index, i.e. be equal to the size of
		 * the index.
		 */
		search_end = search_start + (num - 1);
	} else {
		pthread_rwlock_unlock(&bctl->index_blocks_lock);
		goto out;
	}
	pthread_rwlock_unlock(&bctl->index_blocks_lock);

	st->bsearch_reached++;

	sorted_orig = bsearch(dc, search_start, num, sizeof(struct eblob_disk_control), eblob_disk_control_sort);

	eblob_log(b->cfg.log, EBLOB_LOG_SPAM, "%s: start: %p, end: %p, blob_start: %p, blob_end: %p, num: %zd\n", 
			eblob_dump_id(dc->key.id),
			search_start, search_end, bctl->sort.data, bctl->sort.data + bctl->sort.size, num);

	eblob_log(b->cfg.log, EBLOB_LOG_SPAM, "%s: bsearch range: start: %s, end: %s, num: %zd\n",
			eblob_dump_id(dc->key.id),
			eblob_dump_id(search_start->key.id),
			eblob_dump_id(search_end->key.id), num);

	if (!sorted_orig)
		goto out;

	st->bsearch_found++;

	sorted = sorted_orig;
	while (sorted < end && eblob_disk_control_sort(sorted, dc) == 0) {
		if (callback(sorted, dc)) {
			found = sorted;
			break;
		}
		st->additional_reads++;
		sorted++;
	}

	if (found)
		goto out;

	sorted = sorted_orig - 1;
	while (sorted >= start) {
		st->additional_reads++;
		/*
		 * sorted_orig - 1 at the very beginning may contain different key,
		 * so we change check logic here if compare it with previous loop
		 */
		if (eblob_disk_control_sort(sorted, dc))
			break;

		if (callback(sorted, dc)) {
			found = sorted;
			break;
		}
		sorted--;
	}

out:
	react_stop_action(ACTION_EBLOB_FIND_ON_DISK);
	return found;
}

ssize_t eblob_get_actual_size(int fd)
{
	struct stat st;
	ssize_t err;

	err = fstat(fd, &st);
	if (err < 0)
		return err;

	return st.st_size;
}

int eblob_generate_sorted_index(struct eblob_backend *b, struct eblob_base_ctl *bctl)
{
	struct eblob_map_fd src, dst;
	int fd, err, len;
	char *file, *dst_file;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));

	/* should be enough to store /path/to/data.N.index.sorted */
	len = strlen(b->cfg.file) + sizeof(".index") + sizeof(".sorted") + 256;
	file = malloc(len);
	if (!file) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	dst_file = malloc(len);
	if (!dst_file) {
		err = -ENOMEM;
		goto err_out_free_file;
	}

	snprintf(file, len, "%s-0.%d.index.tmp", b->cfg.file, bctl->index);
	snprintf(dst_file, len, "%s-0.%d.index.sorted", b->cfg.file, bctl->index);

	fd = open(file, O_RDWR | O_TRUNC | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		err = -errno;
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: index: open: index: %d: %s: %s %d\n",
				bctl->index, file, strerror(-err), err);
		goto err_out_free_dst_file;
	}

	src.fd = bctl->index_fd;

	src.size = eblob_get_actual_size(src.fd);
	if (src.size <= 0) {
		err = src.size;
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: index: actual-size: index: %d: %s: %s %d\n",
				bctl->index, file, strerror(-err), err);
		goto err_out_close;
	}

	err = eblob_data_map(&src);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: index: src-map: index: %d, size: %llu: %s: %s %d\n",
				bctl->index, (unsigned long long)src.size, file, strerror(-err), err);
		goto err_out_close;
	}

	dst.fd = fd;
	dst.size = src.size;

	err = eblob_preallocate(dst.fd, 0, dst.size);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: index: eblob_preallocate: index: %d, offset: %llu: %s: %s %d\n",
				bctl->index, (unsigned long long)dst.size, file, strerror(-err), err);
		goto err_out_unmap_src;
	}

	err = eblob_data_map(&dst);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: index: dst-map: index: %d, size: %llu: %s: %s %d\n",
				bctl->index, (unsigned long long)dst.size, file, strerror(-err), err);
		goto err_out_unmap_src;
	}

	memcpy(dst.data, src.data, dst.size);
	qsort(dst.data, dst.size / sizeof(struct eblob_disk_control), sizeof(struct eblob_disk_control),
			eblob_disk_control_sort_with_flags);
	err = msync(dst.data, dst.size, MS_SYNC);
	if (err == -1)
		goto err_out_unmap_dst;

	pthread_mutex_lock(&bctl->lock);
	bctl->sort = dst;
	pthread_mutex_unlock(&bctl->lock);

	eblob_log(b->cfg.log, EBLOB_LOG_INFO, "blob: index: generated sorted: index: %d, "
			"index-size: %llu, data-size: %llu, file: %s\n",
			bctl->index, (unsigned long long)dst.size, (unsigned long long)bctl->data_offset, file);

	rename(file, dst_file);

	eblob_data_unmap(&src);
	free(file);
	free(dst_file);
	return 0;

err_out_unmap_dst:
	eblob_data_unmap(&dst);
err_out_unmap_src:
	eblob_data_unmap(&src);
err_out_close:
	close(fd);
err_out_free_dst_file:
	free(dst_file);
err_out_free_file:
	free(file);
err_out_exit:
	return err;
}

static char *eblob_dump_search_stat(const struct eblob_disk_search_stat *st, int err)
{
	static __thread char ss[1024];

	snprintf(ss, sizeof(ss), "bctls: %d, no-sorted-index: %d, search-on-disk: %d, bloom-no-key: %d, "
			"found-index-block: %d, no-index-block: %d, bsearch-reached: %d, bsearch-found: %d, "
			"additional-reads: %d, err: %d",
			 st->loops, st->no_sort, st->search_on_disk, st->bloom_null,
			 st->found_index_block, st->no_block, st->bsearch_reached, st->bsearch_found,
			 st->additional_reads, err);

	HANDY_GAUGE_SET("eblob.disk.index.lookup.bases", st->loops);
	HANDY_GAUGE_SET("eblob.disk.index.lookup.unsorted", st->no_sort);
	HANDY_GAUGE_SET("eblob.disk.index.lookup.bloom_negative", st->bloom_null);
	HANDY_GAUGE_SET("eblob.disk.index.lookup.bsearch_block.positive", st->found_index_block);
	HANDY_GAUGE_SET("eblob.disk.index.lookup.bsearch_block.negative", st->no_block);
	HANDY_GAUGE_SET("eblob.disk.index.lookup.bsearch_key_miss", st->additional_reads);

	return ss;
}

int eblob_disk_index_lookup(struct eblob_backend *b, struct eblob_key *key,
		struct eblob_ram_control *rctl)
{
	react_start_action(ACTION_EBLOB_DISK_INDEX_LOOKUP);

	HANDY_TIMER_SCOPE("eblob.disk.index.lookup.total", pthread_self());

	struct eblob_base_ctl *bctl;
	struct eblob_disk_control *dc, tmp = { .key = *key, };
	struct eblob_disk_search_stat st = { .bloom_null = 0, };
	static const int max_tries = 10;
	int err = -ENOENT, tries = 0;

	eblob_log(b->cfg.log, EBLOB_LOG_DEBUG, "blob: %s: index: disk.\n", eblob_dump_id(key->id));

again:
	list_for_each_entry_reverse(bctl, &b->bases, base_entry) {
		/* Count number of loops before break */
		++st.loops;
		/* Protect against datasort */
		eblob_bctl_hold(bctl);

		/*
		 * This should be rather rare case when we've grabbed hold of
		 * already invalidated (by data-sort) bctl.
		 * TODO: Actually it's sufficient only to move one bctl back but as
		 * was mentioned - it's really rare case.
		 * TODO: Probably we should check for this inside eblob_bctl_hold()
		 */
		if (bctl->index_fd < 0) {
			eblob_bctl_release(bctl);
			if (tries++ > max_tries)
				return -EDEADLK;
			goto again;
		}

		/* If bctl does not have sorted index - skip it, all its keys are already in ram */
		if (bctl->sort.fd < 0) {
			st.no_sort++;
			eblob_log(b->cfg.log, EBLOB_LOG_DEBUG,
					"blob: %s: index: disk: index: %d: no sorted index\n",
					eblob_dump_id(key->id), bctl->index);
			eblob_bctl_release(bctl);
			continue;
		}

		dc = eblob_find_on_disk(b, bctl, &tmp, eblob_find_non_removed_callback, &st);
		if (dc == NULL) {
			eblob_log(b->cfg.log, EBLOB_LOG_DEBUG,
					"blob: %s: index: disk: index: %d: NO DATA\n",
					eblob_dump_id(key->id), bctl->index);
			eblob_bctl_release(bctl);
			continue;
		}

		eblob_convert_disk_control(dc);
		err = 0;

		memset(rctl, 0, sizeof(*rctl));
		rctl->data_offset = dc->position;
		rctl->index_offset = (void *)dc - bctl->sort.data;
		rctl->size = dc->data_size;
		rctl->bctl = bctl;

		eblob_bctl_release(bctl);

		eblob_log(b->cfg.log, EBLOB_LOG_NOTICE, eblob_dump_id(key->id),
				"blob: %s: index: %d, position: %" PRIu64
				", data_size: %" PRIu64 ": %s\n", eblob_dump_id(key->id),
				rctl->bctl->index, rctl->data_offset, rctl->size, eblob_dump_search_stat(&st, 0));
		break;
	}

	eblob_log(b->cfg.log, EBLOB_LOG_INFO, "blob: %s: stat: %s\n", eblob_dump_id(key->id), eblob_dump_search_stat(&st, 0));


	eblob_stat_add(b->stat, EBLOB_GST_INDEX_READS, st.loops);

	react_stop_action(ACTION_EBLOB_DISK_INDEX_LOOKUP);
	return err;
}
