#ifndef SCRIBE_GROW_VEC_H
#define SCRIBE_GROW_VEC_H

#include <stddef.h>

/*
 * A minimal heap-grown vector for the stitcher's per-aggregate collections.
 *
 * It replaces fixed inline arrays (which made the owning structs large and
 * imposed silent capacity caps) with contiguous storage that grows on demand.
 * Storage stays contiguous and indices stay valid across growth, so callers
 * that slice by index (e.g. a batch's first_source_event_index) keep working.
 *
 * This deliberately is NOT type-erased at the call site: callers cast
 * scribe_grow_vec.data to the concrete element type. The vector only tracks
 * element size so grow() can size the realloc.
 */
typedef struct {
    void *data;
    size_t count;
    size_t capacity;
    size_t elem_size;
} scribe_grow_vec_t;

/* Initialise an empty vector for elements of elem_size bytes. */
void scribe_grow_vec_init(scribe_grow_vec_t *vec, size_t elem_size);

/*
 * Ensure room for one more element, growing (8, then doubling) up to max_count.
 * Returns a pointer to the next free, zeroed slot and increments count, or NULL
 * on allocation failure or when count would exceed max_count. The returned
 * pointer is valid until the next grow; index off vec->data for stable access.
 */
void *scribe_grow_vec_append(scribe_grow_vec_t *vec, size_t max_count);

/* Element pointer at index i (no bounds check; caller guards with count). */
void *scribe_grow_vec_at(const scribe_grow_vec_t *vec, size_t i);

/* Reset count to zero without freeing the buffer (reuse for rebuild paths). */
void scribe_grow_vec_clear(scribe_grow_vec_t *vec);

/* Free the buffer and reset to empty. Safe on a zeroed vector. */
void scribe_grow_vec_free(scribe_grow_vec_t *vec);

#endif
