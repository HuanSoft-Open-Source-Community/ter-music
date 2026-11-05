#ifndef SEGMENT_BUFFER_H
#define SEGMENT_BUFFER_H

#include <stdint.h>
#include <pthread.h>

/* ── Segment constants ── */

#define SEGMENT_DURATION_SEC     15
#define SEGMENT_POOL_SIZE          3    /* current + next + next+1 */
#define SEGMENT_MIN_SPEED       0.75f  /* lowest atempo for worst-case buffer sizing */
#define PRELOAD_SEGMENT_COUNT      1    /* segments to pre-decode for next track */

/* ── Segment: a single 15-second PCM buffer ── */

typedef struct {
    int32_t *data;              /* PCM S32 interleaved data          */
    int capacity_frames;        /* allocated frame capacity           */
    int frame_count;            /* actual decoded frames in segment  */
    int consumed_frames;        /* frames already written to backend */
    int segment_id;             /* global segment index within track */
    int is_valid;               /* 1 = contains decoded data         */
    int is_last;                /* 1 = final segment of the track   */
} Segment;

/* ── SegmentPool: sliding window of 3 segments ── */

typedef struct {
    Segment slots[SEGMENT_POOL_SIZE];  /* 0=current, 1=next, 2=next+1 */
    int current_slot;                   /* index into slots[] for playing segment */
    int current_segment_id;             /* global segment number of current_slot */
    int total_segments;                 /* total segments for this track */
    int sample_rate;
    int channels;
} SegmentPool;

/* ── PreloadData: pre-decoded segments for next track ── */

typedef struct {
    int valid;                              /* 1 = preload data ready       */
    int track_index;                        /* track this was decoded for   */
    Segment segments[PRELOAD_SEGMENT_COUNT]; /* pre-decoded segments       */
    int segment_count;                      /* how many are valid (1-2)    */
    int sample_rate;
    int channels;
    pthread_mutex_t lock;                   /* protects `valid` flag       */
} PreloadData;

/* ====================================================================
 * SegmentPool lifecycle
 * ==================================================================== */

/**
 * @brief Allocate all 3 segment buffers, sized for worst-case atempo speed.
 * @param pool       uninitialised pool (may be garbage)
 * @param sr         output sample rate (e.g. 44100)
 * @param ch         output channels (1 or 2)
 * @return 0 on success, -1 on allocation failure
 */
int  segment_pool_init(SegmentPool *pool, int sr, int ch);

/**
 * @brief Free all segment data buffers and zero the structure.
 */
void segment_pool_destroy(SegmentPool *pool);

/**
 * @brief Reset pool to empty state (preserves buffer memory for reuse).
 */
void segment_pool_reset(SegmentPool *pool);

/* ====================================================================
 * Query helpers
 * ==================================================================== */

static inline int segment_pool_capacity_frames(const SegmentPool *pool)
{
    if (!pool || pool->sample_rate <= 0) return 0;
    return (int)(pool->sample_rate * SEGMENT_DURATION_SEC / SEGMENT_MIN_SPEED);
}

static inline int segment_id_from_position(int position_sec)
{
    if (position_sec < 0) return 0;
    return position_sec / SEGMENT_DURATION_SEC;
}

static inline int segment_start_position(int segment_id)
{
    return segment_id * SEGMENT_DURATION_SEC;
}

static inline int segment_pool_total_for_duration(int duration_sec)
{
    if (duration_sec <= 0) return 1;
    return (duration_sec + SEGMENT_DURATION_SEC - 1) / SEGMENT_DURATION_SEC;
}

/* ====================================================================
 * Pool accessors
 * ==================================================================== */

Segment* segment_pool_current(SegmentPool *pool);
int      segment_pool_current_done(SegmentPool *pool);
int      segment_pool_current_remaining(SegmentPool *pool);
int      segment_pool_is_last(SegmentPool *pool);

/* ====================================================================
 * Pool manipulation
 * ==================================================================== */

/**
 * @brief Advance the sliding window — free slot[current_slot], shift.
 * @return slot id of the freed slot (to decode the next segment into),
 *         or -1 if already at the last segment.
 */
int  segment_pool_advance(SegmentPool *pool);

/**
 * @brief Mark a slot as having valid decoded data.
 * @param pool
 * @param slot_id    index into pool->slots[] (from advance)
 * @param fc         number of valid frames
 * @param seg_id     global segment number within track
 * @param last       1 if final segment
 */
void segment_pool_mark_ready(SegmentPool *pool, int slot_id,
                              int fc, int seg_id, int last);

/**
 * @brief Reset pool, set current_segment_id to target (for seek).
 *        Caller must then decode one segment into current_slot.
 */
void segment_pool_seek_to(SegmentPool *pool, int target_seg);

/**
 * @brief Mark frames as consumed from current segment.
 */
void segment_pool_consume(SegmentPool *pool, int frames);

/**
 * @brief Find the slot farthest from current that is NOT valid.
 * @return slot index, or -1 if all slots are valid/being filled.
 */
int  segment_pool_free_slot(const SegmentPool *pool);

/* ====================================================================
 * PreloadData helpers
 * ==================================================================== */

/**
 * @brief Initialise preload data buffers (allocate once).
 */
int  preload_data_init(PreloadData *pd, int sr, int ch);

/**
 * @brief Lazy init — allocate if not yet, no-op if already valid with matching params.
 */
int  preload_data_ensure_init(PreloadData *pd, int sr, int ch);

/**
 * @brief Free preload data buffers.
 */
void preload_data_destroy(PreloadData *pd);

/** @brief Free preload segment buffers but keep struct reusable (lazy re-init). */
void preload_data_reset(PreloadData *pd);

#endif /* SEGMENT_BUFFER_H */
