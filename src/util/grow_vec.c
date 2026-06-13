#include "grow_vec.h"

#include <stdlib.h>
#include <string.h>

void scribe_grow_vec_init(scribe_grow_vec_t *vec, size_t elem_size)
{
    if (vec == NULL) {
        return;
    }
    vec->data = NULL;
    vec->count = 0u;
    vec->capacity = 0u;
    vec->elem_size = elem_size;
}

void *scribe_grow_vec_append(scribe_grow_vec_t *vec, size_t max_count)
{
    char *slot;

    if (vec == NULL || vec->elem_size == 0u || vec->count >= max_count) {
        return NULL;
    }

    if (vec->count >= vec->capacity) {
        size_t new_capacity = vec->capacity == 0u ? 8u : vec->capacity * 2u;
        void *grown;

        if (new_capacity > max_count) {
            new_capacity = max_count;
        }
        grown = realloc(vec->data, new_capacity * vec->elem_size);
        if (grown == NULL) {
            return NULL;
        }
        vec->data = grown;
        vec->capacity = new_capacity;
    }

    slot = (char *)vec->data + vec->count * vec->elem_size;
    memset(slot, 0, vec->elem_size);
    vec->count++;
    return slot;
}

void *scribe_grow_vec_at(const scribe_grow_vec_t *vec, size_t i)
{
    if (vec == NULL || vec->data == NULL) {
        return NULL;
    }
    return (char *)vec->data + i * vec->elem_size;
}

void scribe_grow_vec_clear(scribe_grow_vec_t *vec)
{
    if (vec != NULL) {
        vec->count = 0u;
    }
}

void scribe_grow_vec_free(scribe_grow_vec_t *vec)
{
    if (vec == NULL) {
        return;
    }
    free(vec->data);
    vec->data = NULL;
    vec->count = 0u;
    vec->capacity = 0u;
}
