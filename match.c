/*
 * Block matching used by the file-transfer code.
 *
 * Copyright (C) 1996 Andrew Tridgell
 * Copyright (C) 1996 Paul Mackerras
 * Copyright (C) 2003-2023 Wayne Davison
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */

#include "rsync.h"
#include "inums.h"

extern int checksum_seed;
extern RSYNC_TLS int append_mode;

extern struct name_num_item *xfer_sum_nni;
extern int xfer_sum_len;

int updating_basis_file;
char sender_file_sum[MAX_DIGEST_LEN];

static int false_alarms;
static int hash_hits;
static int matches;
static int64 data_transfer;

static int total_false_alarms;
static int total_hash_hits;
static int total_matches;

extern RSYNC_TLS struct stats stats;
#define TRADITIONAL_TABLESIZE (1<<16)

/* The maximum number of same-weak-checksum candidates we will compare
 * against at a single file offset before giving up and rolling forward a
 * byte.  A weak checksum that collides thousands of times (very common in
 * disk/VM images, which contain large runs of identical blocks) would
 * otherwise turn hash_search()'s inner loop into an O(file_size *
 * chain_length) scan, pegging a CPU at 100% for hours with no apparent
 * progress (issue #217).
 *
 * Concretely, a synthetic 40000-block basis whose blocks all share one weak
 * checksum took ~18.4s to sync a 60KB source on a modern x86_64 box before
 * this cap and ~0.7s after it -- and the unbounded cost grows with the
 * square of the file size, which is what produced the multi-hour "hangs"
 * reported against real multi-GB images.
 *
 * Capping the per-offset work keeps the search bounded; any block we skip
 * over is simply sent as literal data, so the result is always correct --
 * only the transfer size is (slightly) affected.  This is purely a
 * sender-side search limit: it changes no checksum, emitted byte, or
 * protocol field, so a capped sender interoperates with any receiver. */
#ifndef MAX_CHAIN_LEN
#define MAX_CHAIN_LEN 1024
#endif

/* The sender's lookup table for the receiver's weak checksums.
 *
 * hash_search() probes this once per byte of unmatched data, so its layout
 * decides how fast a delta transfer can run through changed regions.  The
 * original design -- a 16-bit hash of sum1 into an int32 table, then a walk
 * down a chain of sum_buf entries comparing the full sum1 -- costs two
 * data-dependent cache misses per byte once the file has more than a few
 * thousand blocks: one into the table and one into the chain, because the
 * table itself cannot tell whether any block really has this sum1.  On a
 * 20 GB disk image (160k blocks, a 1 MB table, a 4 MB sums array) that is
 * what pins a laptop core at 100% for ~30 MB/s.
 *
 * Instead, an open-addressed table keyed on the full 32-bit sum1: each slot
 * holds the sum1 and the head of a chain that contains ONLY blocks with that
 * exact sum1.  A miss -- the overwhelmingly common case on changed data --
 * is settled by the one slot, and hash_search() prefetches that slot a few
 * dozen bytes ahead of time (see LOOKAHEAD), so the miss no longer waits on
 * memory at all.  Blocks with equal weak checksums (runs of zeros in disk
 * images produce thousands) still chain, most recent first, exactly as
 * before, and the --inplace pruning still unlinks through the head pointer.
 *
 * This is a sender-side data structure only: it changes which candidate
 * blocks are compared, in which order, and nothing on the wire. */
struct hash_slot {
	uint32 sum1;	/* weak checksum shared by every block on the chain */
	int32 idx;	/* first block with that sum1; HASH_EMPTY if never used */
};

/* A chain that --inplace pruning has emptied is left as a tombstone with
 * idx == -1: it must keep stopping nobody, since keys inserted after it
 * probed past it.  Only HASH_EMPTY ends a probe. */
#define HASH_EMPTY (-2)

static struct hash_slot *hash_table;
static uint32 hash_mask;	/* tablesize - 1; tablesize is a power of two */
static int hash_shift;		/* 32 - log2(tablesize) */

/* Fibonacci hashing: one multiply, and the high bits it keeps are the ones
 * every input bit influences, so sum1 values that differ only in the low
 * byte (adjacent-looking data) still spread across the table. */
#define SUM1HASH(sum) ((uint32)((sum) * 0x9E3779B1u) >> hash_shift)

#define MIN_TABLESIZE 1024

static void build_hash_table(struct sum_struct *s)
{
	static uint32 alloc_size;
	uint32 tablesize, want;
	int32 i;

	/* A power of two at least twice the block count: a load factor of at
	 * most 1/2 keeps the linear-probe runs short, and strictly more slots
	 * than blocks guarantees the probe in hash_search() always reaches an
	 * empty slot.  The cap keeps tablesize from wrapping to zero for a
	 * (theoretical) count near INT32_MAX; even then count < tablesize. */
	want = (uint32)s->count * 2 + 1;
	tablesize = MIN_TABLESIZE;
	while (tablesize < want && tablesize < 0x80000000u)
		tablesize <<= 1;
	hash_mask = tablesize - 1;
	hash_shift = 32;
	for (want = tablesize; want > 1; want >>= 1)
		hash_shift--;

	if (tablesize != alloc_size) {
		if (hash_table)
			free(hash_table);
		hash_table = new_array(struct hash_slot, tablesize);
		alloc_size = tablesize;
	}

	for (i = 0; (uint32)i < tablesize; i++)
		hash_table[i].idx = HASH_EMPTY;

	for (i = 0; i < s->count; i++) {
		uint32 sum = s->sums[i].sum1;
		uint32 h = SUM1HASH(sum);
		while (hash_table[h].idx != HASH_EMPTY && hash_table[h].sum1 != sum)
			h = (h + 1) & hash_mask;
		if (hash_table[h].idx == HASH_EMPTY) {
			hash_table[h].sum1 = sum;
			hash_table[h].idx = -1;
		}
		/* Most recent block first, as the chained table did. */
		s->sums[i].chain = hash_table[h].idx;
		hash_table[h].idx = i;
	}
}


static OFF_T last_match;


/* Transmit a literal and/or match token.
 *
 * This delightfully-named function is called either when we find a
 * match and need to transmit all the unmatched data leading up to it,
 * or when we get bored of accumulating literal data and just need to
 * transmit it.  As a result of this second case, it is called even if
 * we have not matched at all!
 *
 * If i >= 0, the number of a matched token.  If < 0, indicates we have
 * only literal data.  A -1 will send a 0-token-int too, and a -2 sends
 * only literal data, w/o any token-int. */
static void matched(int f, struct sum_struct *s, struct map_struct *buf, OFF_T offset, int32 i)
{
	int32 n = (int32)(offset - last_match); /* max value: block_size (int32) */
	int32 j;

	if (DEBUG_GTE(DELTASUM, 2) && i >= 0) {
		rprintf(FINFO,
			"match at %s last_match=%s j=%d len=%ld n=%ld\n",
			big_num(offset), big_num(last_match), i,
			(long)s->sums[i].len, (long)n);
	}

	send_token(f, i, buf, last_match, n, i < 0 ? 0 : s->sums[i].len);
	data_transfer += n;

	if (i >= 0) {
		stats.matched_data += s->sums[i].len;
		n += s->sums[i].len;
	}

	for (j = 0; j < n; j += CHUNK_SIZE) {
		int32 n1 = MIN(CHUNK_SIZE, n - j);
		sum_update(map_ptr(buf, last_match + j, n1), n1);
	}

	if (i >= 0)
		last_match = offset + s->sums[i].len;
	else
		last_match = offset;

	if (buf && INFO_GTE(PROGRESS, 1))
		show_progress(last_match, buf->file_size);
}


/* hash_search() rolls the weak checksum forward one byte at a time through
 * data that does not match a block, looking the sum up in hash_table at
 * every offset.  Two things make that loop cheap:
 *
 * LOOKAHEAD: a second rolling checksum runs LOOKAHEAD bytes ahead of the
 * real one, and the table slot for ITS value is prefetched.  By the time
 * the real search reaches that offset the slot is in L1, and the loop no
 * longer stalls on a cache miss per byte.  The lookahead is only bookkeeping
 * -- it never decides a match -- so its window can be dropped and rebuilt
 * with get_checksum1() whenever the search jumps (after a match, or near
 * the end of the file where a full-size window ahead does not exist).
 *
 * MAP_HOIST: map_ptr() is asked for the block plus up to MAP_HOIST bytes
 * beyond it, and the pointer is then walked directly for that many rolls,
 * instead of calling map_ptr() once per byte (a fifth of the sender's time,
 * measured).  `avail' counts how many rolls the pointer is good for.  Any
 * other map_ptr() call in the loop -- a checksum, a flushed literal -- sets
 * avail = 0, which forces the next roll to fetch the window afresh rather
 * than trust that the window did not move. */
#define LOOKAHEAD 32
#define MAP_HOIST (256 * 1024)

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#define PREFETCH(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#elif defined(__GNUC__)
#define PREFETCH(p) __builtin_prefetch(p)
#else
#define PREFETCH(p) do { } while (0)
#endif

/* Look for a block of the basis file at exactly `pos' in the sender's file.
 * Returns the block's index, or -1.  This is the same test hash_search()
 * applies while rolling, minus the --inplace bookkeeping (callers skip the
 * probe in that mode), and it prefers `want_i', the block the previous
 * match predicts, so the RLL coder keeps seeing adjacent tokens.
 * `map' must already cover [pos, pos + blength) -- see the caller. */
static int32 probe_block(struct sum_struct *s, schar *map, OFF_T len, OFF_T pos,
			 int32 want_i, char *sum2)
{
	int32 l = (int32)MIN((OFF_T)s->blength, len - pos);
	uint32 sum = get_checksum1((char *)map, l);
	uint32 h = SUM1HASH(sum);
	int32 i, chain_len = 0;
	int done_csum2 = 0;

	for (;;) {
		i = hash_table[h].idx;
		if (i == HASH_EMPTY)
			return -1;
		if (hash_table[h].sum1 == sum)
			break;
		h = (h + 1) & hash_mask;
	}

	if (want_i < s->count && s->sums[want_i].sum1 == sum && s->sums[want_i].len == l) {
		get_checksum2((char *)map, l, sum2);
		done_csum2 = 1;
		if (memcmp(sum2, sum2_at(s, want_i), s->s2length) == 0)
			return want_i;
	}

	for (; i >= 0; i = s->sums[i].chain) {
		if (++chain_len > MAX_CHAIN_LEN)
			return -1;
		if (s->sums[i].len != l)
			continue;
		if (!done_csum2) {
			get_checksum2((char *)map, l, sum2);
			done_csum2 = 1;
		}
		if (memcmp(sum2, sum2_at(s, i), s->s2length) == 0)
			return i;
		false_alarms++;
	}
	return -1;
}

/* How many blocks past a broken one hash_search() tries at their aligned
 * position before it falls back to rolling through the data byte by byte.
 *
 * Files that are modified in place -- disk images, databases, anything a
 * program rewrites sectors of -- keep every unchanged block exactly where
 * it was, so after a block fails to match, the block after it is almost
 * always sitting at offset + blength.  The classic search only finds that
 * by rolling the checksum through the whole broken block, one byte at a
 * time, and that roll is what makes a delta transfer of a disk image run
 * at a small fraction of the line rate.  A probe is one get_checksum1()
 * over the block, roughly 15x cheaper per byte than rolling, so a few of
 * them cost less than rolling through a single block.
 *
 * What it gives up: when a probe succeeds, any match that a byte-by-byte
 * roll would have found INSIDE the skipped blocks -- a shifted copy of some
 * basis block -- is sent as literal data instead.  That needs an insertion
 * and a matching deletion within a few blocks of each other, and costs at
 * most those blocks in transfer size, never correctness.  When every probe
 * fails (shifted data, appended data, a wholly different file), the search
 * rolls on exactly as before, and the probes are not retried until the
 * next match, so a file that never matches pays for them exactly once.
 *
 * Sixteen, because a failed round then costs about what rolling through
 * one block does (the roll is ~40x the work per byte on a laptop core),
 * while a disk image whose guest wrote every second sector still gets
 * runs of half a dozen consecutive changed blocks that a shorter round
 * would leave to the roll. */
#define ALIGNED_PROBES 16

static void hash_search(int f,struct sum_struct *s,
			struct map_struct *buf, OFF_T len)
{
	OFF_T offset, aligned_offset, end;
	int32 k, want_i, aligned_i, backup;
	char sum2[MAX_DIGEST_LEN];
	uint32 s1, s2, sum;
	uint32 a1 = 0, a2 = 0;	/* the lookahead window's rolling checksum */
	int ahead_valid = 0;
	int32 avail = 0;	/* rolls the current map pointer is good for */
	int more;
	schar *map;

	// prevent possible memory leaks
	memset(sum2, 0, sizeof sum2);

	/* want_i is used to encourage adjacent matches, allowing the RLL
	 * coding of the output to work more efficiently. */
	want_i = 0;

	if (DEBUG_GTE(DELTASUM, 2)) {
		rprintf(FINFO, "hash search b=%ld len=%s\n",
			(long)s->blength, big_num(len));
	}

	k = (int32)MIN(len, (OFF_T)s->blength);

	map = (schar *)map_ptr(buf, 0, k);

	sum = get_checksum1((char *)map, k);
	s1 = sum & 0xFFFF;
	s2 = sum >> 16;
	if (DEBUG_GTE(DELTASUM, 3))
		rprintf(FINFO, "sum=%.8x k=%ld\n", sum, (long)k);

	offset = aligned_offset = aligned_i = 0;

	end = len + 1 - s->sums[s->count-1].len;

	if (DEBUG_GTE(DELTASUM, 3)) {
		rprintf(FINFO, "hash search s->blength=%ld len=%s count=%s\n",
			(long)s->blength, big_num(len), big_num(s->count));
	}

	do {
		int done_csum2 = 0;
		uint32 hash_entry;
		int32 i, *prev;
		int32 chain_len = 0;

		if (DEBUG_GTE(DELTASUM, 4)) {
			rprintf(FINFO, "offset=%s sum=%04x%04x\n",
				big_num(offset), s2 & 0xFFFF, s1 & 0xFFFF);
		}

		sum = (s1 & 0xffff) | (s2 << 16);
		hash_entry = SUM1HASH(sum);
		for (;;) {
			i = hash_table[hash_entry].idx;
			if (i == HASH_EMPTY)
				goto null_hash;
			if (hash_table[hash_entry].sum1 == sum)
				break;
			hash_entry = (hash_entry + 1) & hash_mask;
		}
		if (i < 0)	/* a chain that --inplace pruning emptied */
			goto null_hash;
		prev = &hash_table[hash_entry].idx;

		hash_hits++;
		do {
			int32 l;

			/* When updating in-place, the chunk's offset must be
			 * either >= our offset or identical data at that offset.
			 * Remove any bypassed entries that we can never use. */
			if (updating_basis_file && s->sums[i].offset < offset
			 && !(s->sums[i].flags & SUMFLG_SAME_OFFSET)) {
				*prev = s->sums[i].chain;
				continue;
			}
			prev = &s->sums[i].chain;

			if (sum != s->sums[i].sum1)
				continue;

			/* Bound the work spent on a single pathological hash
			 * bucket.  If this weak checksum matches more than
			 * MAX_CHAIN_LEN records, stop scanning and treat this
			 * offset as a non-match (issue #217).  The skipped data
			 * is sent literally, never corrupted. */
			if (++chain_len > MAX_CHAIN_LEN)
				break;

			/* also make sure the two blocks are the same length */
			l = (int32)MIN((OFF_T)s->blength, len-offset);
			if (l != s->sums[i].len)
				continue;

			if (DEBUG_GTE(DELTASUM, 3)) {
				rprintf(FINFO,
					"potential match at %s i=%ld sum=%08x\n",
					big_num(offset), (long)i, sum);
			}

			if (!done_csum2) {
				map = (schar *)map_ptr(buf,offset,l);
				avail = 0;
				get_checksum2((char *)map,l,sum2);
				done_csum2 = 1;
			}

			if (memcmp(sum2, sum2_at(s, i), s->s2length) != 0) {
				false_alarms++;
				continue;
			}

			/* When updating in-place, the best possible match is
			 * one with an identical offset, so we prefer that over
			 * the adjacent want_i optimization. */
			if (updating_basis_file) {
				/* All the generator's chunks start at blength boundaries. */
				while (aligned_offset < offset) {
					aligned_offset += s->blength;
					aligned_i++;
				}
				if ((offset == aligned_offset
				  || (sum == 0 && l == s->blength && aligned_offset + l <= len))
				 && aligned_i < s->count) {
					if (i != aligned_i) {
						if (sum != s->sums[aligned_i].sum1
						 || l != s->sums[aligned_i].len
						 || memcmp(sum2, sum2_at(s, aligned_i), s->s2length) != 0)
							goto check_want_i;
						i = aligned_i;
					}
					if (offset != aligned_offset) {
						/* We've matched some zeros in a spot that is also zeros
						 * further along in the basis file, if we find zeros ahead
						 * in the sender's file, we'll output enough literal data
						 * to re-align with the basis file, and get back to seeking
						 * instead of writing. */
						backup = (int32)(aligned_offset - last_match);
						if (backup < 0)
							backup = 0;
						map = (schar *)map_ptr(buf, aligned_offset - backup, l + backup)
						    + backup;
						avail = 0;
						sum = get_checksum1((char *)map, l);
						if (sum != s->sums[i].sum1)
							goto check_want_i;
						get_checksum2((char *)map, l, sum2);
						if (memcmp(sum2, sum2_at(s, i), s->s2length) != 0)
							goto check_want_i;
						/* OK, we have a re-alignment match.  Bump the offset
						 * forward to the new match point. */
						offset = aligned_offset;
					}
					/* This identical chunk is in the same spot in the old and new file. */
					s->sums[i].flags |= SUMFLG_SAME_OFFSET;
					want_i = i;
				}
			}

		  check_want_i:
			/* we've found a match, but now check to see
			 * if want_i can hint at a better match. */
			if (i != want_i && want_i < s->count
			 && (!updating_basis_file || s->sums[want_i].offset >= offset
			  || s->sums[want_i].flags & SUMFLG_SAME_OFFSET)
			 && sum == s->sums[want_i].sum1
			 && l == s->sums[want_i].len
			 && memcmp(sum2, sum2_at(s, want_i), s->s2length) == 0) {
				/* we've found an adjacent match - the RLL coder
				 * will be happy */
				i = want_i;
			}
			want_i = i + 1;

			matched(f,s,buf,offset,i);
			offset += s->sums[i].len - 1;
			k = (int32)MIN((OFF_T)s->blength, len-offset);
			map = (schar *)map_ptr(buf, offset, k);
			avail = 0;
			ahead_valid = 0;
			sum = get_checksum1((char *)map, k);
			s1 = sum & 0xFFFF;
			s2 = sum >> 16;
			matches++;
			break;
		} while ((i = s->sums[i].chain) >= 0);

	  null_hash:
		backup = (int32)(offset - last_match);
		/* We sometimes read 1 byte prior to last_match... */
		if (backup < 0)
			backup = 0;

		if (offset == last_match && !updating_basis_file && k == s->blength) {
			/* We are right behind a match (or at the start) and the
			 * block here does not match: before rolling through it,
			 * try the next few blocks at their aligned positions.
			 * One window covers the probes and the roll that may
			 * follow, so failed probes do not thrash map_ptr(). */
			OFF_T want = (OFF_T)k + (OFF_T)ALIGNED_PROBES * s->blength;
			int32 wlen = (int32)MIN(len - offset, want);
			schar *base = (schar *)map_ptr(buf, offset, wlen);
			int j;
			avail = 0;
			for (j = 1; j <= ALIGNED_PROBES; j++) {
				OFF_T p = offset + (OFF_T)j * s->blength;
				int32 i;
				if (p >= end)
					break;
				i = probe_block(s, base + (OFF_T)j * s->blength, len, p, want_i, sum2);
				if (i < 0)
					continue;
				/* Send the skipped blocks as literal data and pick
				 * up after the match, just as a rolled match does. */
				want_i = i + 1;
				matched(f, s, buf, p, i);
				offset = p + s->sums[i].len - 1;
				k = (int32)MIN((OFF_T)s->blength, len-offset);
				map = (schar *)map_ptr(buf, offset, k);
				sum = get_checksum1((char *)map, k);
				s1 = sum & 0xFFFF;
				s2 = sum >> 16;
				matches++;
				ahead_valid = 0;
				backup = 0;
				break;
			}
		}

		if (avail <= LOOKAHEAD) {
			/* Fetch the window covering everything from last_match
			 * (so the literal flush below reads what is already
			 * mapped) through the block and as far past it as the
			 * lookahead and hoisting can use.  Past the file's end
			 * there is nothing to roll in, and `more' is 0. */
			OFF_T room = len - (offset + k);
			if (room > MAP_HOIST)
				room = MAP_HOIST;
			avail = room > 0 ? (int32)room : 0;
			map = (schar *)map_ptr(buf, offset - backup, k + avail + backup) + backup;
			if (avail > LOOKAHEAD) {
				if (!ahead_valid) {
					uint32 asum = get_checksum1((char *)map + LOOKAHEAD, k);
					a1 = asum & 0xFFFF;
					a2 = asum >> 16;
					ahead_valid = 1;
				}
			} else
				ahead_valid = 0;
		}
		more = avail > 0;

		if (ahead_valid) {
			/* Roll the lookahead window one byte and prefetch the
			 * slot its checksum will probe LOOKAHEAD rolls from now. */
			a1 -= map[LOOKAHEAD] + CHAR_OFFSET;
			a2 -= k * (map[LOOKAHEAD] + CHAR_OFFSET);
			a1 += map[LOOKAHEAD + k] + CHAR_OFFSET;
			a2 += a1;
			PREFETCH(&hash_table[SUM1HASH((a1 & 0xffff) | (a2 << 16))]);
		}

		/* Trim off the first byte from the checksum */
		s1 -= map[0] + CHAR_OFFSET;
		s2 -= k * (map[0]+CHAR_OFFSET);

		/* Add on the next byte (if there is one) to the checksum */
		if (more) {
			s1 += map[k] + CHAR_OFFSET;
			s2 += s1;
			avail--;
		} else
			--k;
		map++;

		/* By matching early we avoid re-reading the
		   data 3 times in the case where a token
		   match comes a long way after last
		   match. The 3 reads are caused by the
		   running match, the checksum update and the
		   literal send. */
		if (backup >= s->blength+CHUNK_SIZE && end-offset > CHUNK_SIZE) {
			matched(f, s, buf, offset - s->blength, -2);
			avail = 0;
		}
	} while (++offset < end);

	matched(f, s, buf, len, -1);
	map_ptr(buf, len-1, 1);
}


/**
 * Scan through a origin file, looking for sections that match
 * checksums from the generator, and transmit either literal or token
 * data.
 *
 * Also calculates the MD4 checksum of the whole file, using the md
 * accumulator.  This is transmitted with the file as protection
 * against corruption on the wire.
 *
 * @param s Checksums received from the generator.  If <tt>s->count ==
 * 0</tt>, then there are actually no checksums for this file.
 *
 * @param len Length of the file to send.
 **/
void match_sums(int f, struct sum_struct *s, struct map_struct *buf, OFF_T len)
{
	last_match = 0;
	false_alarms = 0;
	hash_hits = 0;
	matches = 0;
	data_transfer = 0;

	sum_init(xfer_sum_nni, checksum_seed);

	if (append_mode > 0) {
		if (s->flength > len) {
			/* A hostile or confused peer can claim a verified-prefix
			 * length that exceeds what we have on disk -- including
			 * for an empty local file, where buf is NULL and the
			 * map_ptr() calls below would dereference it.  Clamp to
			 * what we can actually read. */
			s->flength = len;
		}
		if (append_mode == 2) {
			OFF_T j = 0;
			for (j = CHUNK_SIZE; j < s->flength; j += CHUNK_SIZE) {
				if (buf && INFO_GTE(PROGRESS, 1))
					show_progress(last_match, buf->file_size);
				sum_update(map_ptr(buf, last_match, CHUNK_SIZE),
					   CHUNK_SIZE);
				last_match = j;
			}
			if (last_match < s->flength) {
				int32 n = (int32)(s->flength - last_match);
				if (buf && INFO_GTE(PROGRESS, 1))
					show_progress(last_match, buf->file_size);
				sum_update(map_ptr(buf, last_match, n), n);
			}
		}
		last_match = s->flength;
		s->count = 0;
	}

	if (len > 0 && s->count > 0) {
		build_hash_table(s);

		if (DEBUG_GTE(DELTASUM, 2))
			rprintf(FINFO,"built hash table\n");

		hash_search(f, s, buf, len);

		if (DEBUG_GTE(DELTASUM, 2))
			rprintf(FINFO,"done hash search\n");
	} else {
		OFF_T j;
		/* by doing this in pieces we avoid too many seeks */
		for (j = last_match + CHUNK_SIZE; j < len; j += CHUNK_SIZE)
			matched(f, s, buf, j, -2);
		matched(f, s, buf, len, -1);
	}

	sum_end(sender_file_sum);

	/* If we had a read error, send a bad checksum.  We use all bits
	 * off as long as the checksum doesn't happen to be that, in
	 * which case we turn the last 0 bit into a 1. */
	if (buf && buf->status != 0) {
		int i;
		for (i = 0; i < xfer_sum_len && sender_file_sum[i] == 0; i++) {}
		memset(sender_file_sum, 0, xfer_sum_len);
		if (i == xfer_sum_len)
			sender_file_sum[i-1]++;
	}

	if (DEBUG_GTE(DELTASUM, 2))
		rprintf(FINFO,"sending file_sum\n");
	write_buf(f, sender_file_sum, xfer_sum_len);

	if (DEBUG_GTE(DELTASUM, 2)) {
		rprintf(FINFO, "false_alarms=%d hash_hits=%d matches=%d\n",
			false_alarms, hash_hits, matches);
	}

	total_hash_hits += hash_hits;
	total_false_alarms += false_alarms;
	total_matches += matches;
	stats.literal_data += data_transfer;
}

void match_report(void)
{
	if (!DEBUG_GTE(DELTASUM, 1))
		return;

	rprintf(FINFO,
		"total: matches=%d  hash_hits=%d  false_alarms=%d data=%s\n",
		total_matches, total_hash_hits, total_false_alarms,
		big_num(stats.literal_data));
}
