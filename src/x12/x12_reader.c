#include "x12_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int x12_document_load(const char *path, x12_document_t *doc)
{
    FILE *fp;
    long file_size;
    size_t read_size;
    char *buffer;

    if (path == NULL || doc == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(doc, 0, sizeof(*doc));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return X12_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        (void)fclose(fp);
        return X12_ERR_IO;
    }

    file_size = ftell(fp);
    if (file_size < 0) {
        (void)fclose(fp);
        return X12_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        (void)fclose(fp);
        return X12_ERR_IO;
    }

    buffer = (char *)malloc((size_t)file_size + 1u);
    if (buffer == NULL) {
        (void)fclose(fp);
        return X12_ERR_NO_MEMORY;
    }

    read_size = fread(buffer, 1u, (size_t)file_size, fp);
    if (read_size != (size_t)file_size) {
        free(buffer);
        (void)fclose(fp);
        return X12_ERR_IO;
    }

    if (fclose(fp) != 0) {
        free(buffer);
        return X12_ERR_IO;
    }

    buffer[read_size] = '\0';
    doc->buffer = buffer;
    doc->buffer_len = read_size;
    doc->delimiters.element_sep = '\0';
    doc->delimiters.component_sep = '\0';
    doc->delimiters.segment_term = '\0';

    return X12_OK;
}

void x12_document_free(x12_document_t *doc)
{
    if (doc == NULL) {
        return;
    }

    free(doc->buffer);
    memset(doc, 0, sizeof(*doc));
}
