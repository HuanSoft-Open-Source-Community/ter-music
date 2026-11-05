/**
 * @file segment_buffer.c
 * @brief 分段音频缓冲区 — SegmentPool 与预加载数据管理
 *
 * 将曲目拆成 15 秒一段，池中最多保留 3 段（当前 + 后两段），
 * 并在曲目末尾预加载下一曲的前 1-2 段。
 * 参考 m3u8/HLS 分段思路，适用于 ter-music 播放器。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-04
 */

#include "audio/segment_buffer.h"
#include "logger/logger.h"
#include <stdlib.h>
#include <string.h>

/* ==================================================================
 * Internal helpers
 * ================================================================== */

/**
 * @brief Allocate one segment's buffer.
 * @param seg   pointer to Segment (must not be NULL, may contain garbage)
 * @param cap   desired frame capacity
 * @param ch    channels
 * @return 0 on success, -1 on allocation failure
 */
static int segment_alloc(Segment *seg, int cap, int ch)
{
    if (!seg || cap <= 0 || ch <= 0) return -1;
    size_t sz = (size_t)cap * (size_t)ch * sizeof(int32_t);
    seg->data = malloc(sz);
    if (!seg->data) {
        log_error("segment", "calloc(%zu) failed for segment buffer", sz);
        return -1;
    }
    seg->capacity_frames = cap;
    seg->frame_count     = 0;
    seg->consumed_frames = 0;
    seg->segment_id      = -1;
    seg->is_valid        = 0;
    seg->is_last         = 0;
    return 0;
}

static void segment_free(Segment *seg)
{
    if (!seg) return;
    free(seg->data);
    seg->data            = NULL;
    seg->capacity_frames = 0;
    seg->frame_count     = 0;
    seg->consumed_frames = 0;
    seg->segment_id      = -1;
    seg->is_valid        = 0;
    seg->is_last         = 0;
}

/* ==================================================================
 * SegmentPool lifecycle
 * ================================================================== */

int segment_pool_init(SegmentPool *pool, int sr, int ch)
{
    if (!pool || sr <= 0 || (ch != 1 && ch != 2)) return -1;

    memset(pool, 0, sizeof(*pool));
    int cap = (int)(sr * SEGMENT_DURATION_SEC / SEGMENT_MIN_SPEED);
    if (cap <= 0) return -1;

    for (int i = 0; i < SEGMENT_POOL_SIZE; i++) {
        if (segment_alloc(&pool->slots[i], cap, ch) < 0) {
            for (int j = 0; j < i; j++) segment_free(&pool->slots[j]);
            return -1;
        }
    }

    pool->current_slot      = 0;
    pool->current_segment_id = 0;
    pool->total_segments    = 1;
    pool->sample_rate       = sr;
    pool->channels          = ch;

    log_debug("segment", "pool init: sr=%d ch=%d cap_frames=%d", sr, ch, cap);
    return 0;
}

void segment_pool_destroy(SegmentPool *pool)
{
    if (!pool) return;
    for (int i = 0; i < SEGMENT_POOL_SIZE; i++)
        segment_free(&pool->slots[i]);
    memset(pool, 0, sizeof(*pool));
}

void segment_pool_reset(SegmentPool *pool)
{
    if (!pool) return;
    for (int i = 0; i < SEGMENT_POOL_SIZE; i++) {
        pool->slots[i].frame_count     = 0;
        pool->slots[i].consumed_frames = 0;
        pool->slots[i].segment_id      = -1;
        pool->slots[i].is_valid        = 0;
        pool->slots[i].is_last         = 0;
    }
    pool->current_slot       = 0;
    pool->current_segment_id = 0;
    /* NOT resetting total_segments — it's a track property */
}

/* ==================================================================
 * Pool accessors
 * ================================================================== */

Segment* segment_pool_current(SegmentPool *pool)
{
    if (!pool) return NULL;
    Segment *seg = &pool->slots[pool->current_slot];
    return seg->is_valid ? seg : NULL;
}

int segment_pool_current_done(SegmentPool *pool)
{
    Segment *seg = segment_pool_current(pool);
    if (!seg) return 1;                     /* no valid segment → done */
    return seg->consumed_frames >= seg->frame_count;
}

int segment_pool_current_remaining(SegmentPool *pool)
{
    Segment *seg = segment_pool_current(pool);
    if (!seg) return 0;
    int rem = seg->frame_count - seg->consumed_frames;
    return rem > 0 ? rem : 0;
}

int segment_pool_is_last(SegmentPool *pool)
{
    if (!pool) return 1;
    Segment *seg = segment_pool_current(pool);
    if (!seg) return 0;
    return seg->is_last;
}

/* ==================================================================
 * Pool manipulation
 * ================================================================== */

int segment_pool_advance(SegmentPool *pool)
{
    if (!pool) return -1;

    /* Check if we've exhausted all segments */
    if (pool->current_segment_id + 1 >= pool->total_segments)
        return -1;

    /* Free the current slot */
    Segment *cur = &pool->slots[pool->current_slot];
    cur->frame_count     = 0;
    cur->consumed_frames = 0;
    cur->segment_id      = -1;
    cur->is_valid        = 0;
    cur->is_last         = 0;

    /* Move current_slot forward (circular) */
    int old_slot = pool->current_slot;
    pool->current_slot = (pool->current_slot + 1) % SEGMENT_POOL_SIZE;
    pool->current_segment_id++;

    /* The newly-vacated slot is now the farthest from current → free for reuse */
    return old_slot;
}

void segment_pool_mark_ready(SegmentPool *pool, int slot_id,
                              int fc, int seg_id, int last)
{
    if (!pool || slot_id < 0 || slot_id >= SEGMENT_POOL_SIZE) return;
    Segment *seg = &pool->slots[slot_id];
    seg->frame_count     = fc;
    seg->consumed_frames = 0;
    seg->segment_id      = seg_id;
    seg->is_last         = last;
    seg->is_valid        = 1;
}

void segment_pool_seek_to(SegmentPool *pool, int target_seg)
{
    if (!pool) return;
    segment_pool_reset(pool);
    pool->current_segment_id = target_seg;
}

void segment_pool_consume(SegmentPool *pool, int frames)
{
    Segment *seg = segment_pool_current(pool);
    if (seg && frames > 0)
        seg->consumed_frames += frames;
}

int segment_pool_free_slot(const SegmentPool *pool)
{
    if (!pool) return -1;

    /* If current slot itself is invalid, return it first so Phase 3 can fill it.
     * This prevents getting stuck when the current slot was never decoded. */
    if (!pool->slots[pool->current_slot].is_valid)
        return pool->current_slot;

    /* Look for the first invalid slot, starting from farthest from current */
    for (int offset = 1; offset < SEGMENT_POOL_SIZE; offset++) {
        int idx = (pool->current_slot + offset) % SEGMENT_POOL_SIZE;
        if (!pool->slots[idx].is_valid)
            return idx;
    }
    return -1;   /* all slots are valid */
}

/* ==================================================================
 * PreloadData
 * ================================================================== */

/**
 * @brief Ensure preload data buffers are allocated.
 *        Safe to call multiple times — no-op if already initialised.
 */
int preload_data_ensure_init(PreloadData *pd, int sr, int ch)
{
    if (!pd) return -1;
    /* Already initialised with matching parameters */
    if (pd->segments[0].data && pd->sample_rate == sr && pd->channels == ch)
        return 0;
    /* Already initialised with different parameters — re-allocate */
    if (pd->segments[0].data)
        preload_data_destroy(pd);
    return preload_data_init(pd, sr, ch);
}

int preload_data_init(PreloadData *pd, int sr, int ch)
{
    if (!pd || sr <= 0 || (ch != 1 && ch != 2)) return -1;

    memset(pd, 0, sizeof(*pd));
    int cap = (int)(sr * SEGMENT_DURATION_SEC / SEGMENT_MIN_SPEED);
    if (cap <= 0) return -1;

    for (int i = 0; i < PRELOAD_SEGMENT_COUNT; i++) {
        if (segment_alloc(&pd->segments[i], cap, ch) < 0) {
            for (int j = 0; j < i; j++) segment_free(&pd->segments[j]);
            return -1;
        }
    }

    pd->valid         = 0;
    pd->track_index   = -1;
    pd->segment_count = 0;
    pd->sample_rate   = sr;
    pd->channels      = ch;
    pthread_mutex_init(&pd->lock, NULL);
    return 0;
}

void preload_data_destroy(PreloadData *pd)
{
    if (!pd) return;
    for (int i = 0; i < PRELOAD_SEGMENT_COUNT; i++)
        segment_free(&pd->segments[i]);
    pthread_mutex_destroy(&pd->lock);
    memset(pd, 0, sizeof(*pd));
}

void preload_data_reset(PreloadData *pd)
{
    if (!pd) return;
    for (int i = 0; i < PRELOAD_SEGMENT_COUNT; i++)
        segment_free(&pd->segments[i]);
    pd->valid         = 0;
    pd->track_index   = -1;
    pd->segment_count = 0;
    pd->sample_rate   = 0;
    pd->channels      = 0;
}

/* ==================================================================
 * Shared global for next-track preloading
 * ================================================================== */

PreloadData g_preload_data = {0};
