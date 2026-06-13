#include "x12_mapper_phi.h"

#include <string.h>

x12_str_t x12_mapper_empty_str(void)
{
    x12_str_t value;

    value.ptr = "";
    value.len = 0;
    return value;
}

x12_str_t x12_mapper_element_or_empty(const x12_segment_t *seg, size_t index)
{
    return seg->element_count <= index ? x12_mapper_empty_str() : seg->elements[index];
}

int x12_mapper_add_phi_str(event_writer_t *writer, const char *name, x12_str_t value)
{
    if (!event_writer_include_phi(writer))
    {
        return X12_OK;
    }
    return event_writer_add_str(writer, name, value);
}

int x12_mapper_add_phi_token(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;

    if (!event_writer_include_phi(writer))
    {
        return X12_OK;
    }
    if (raw.len == 0u)
    {
        return event_writer_add_str(writer, name, x12_mapper_empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(writer, name, token_value);
}

int x12_mapper_add_tokenized_or_phi(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;

    if (event_writer_include_phi(writer))
    {
        TRY(event_writer_record_phi_mapping(writer, type, raw));
        return event_writer_add_str(writer, name, raw);
    }

    if (raw.len == 0u)
    {
        return event_writer_add_str(writer, name, x12_mapper_empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    TRY(event_writer_record_phi_mapping(writer, type, raw));

    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(writer, name, token_value);
}
