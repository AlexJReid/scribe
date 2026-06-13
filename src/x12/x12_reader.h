#ifndef SCRIBE_X12_READER_H
#define SCRIBE_X12_READER_H

#include "x12_parser.h"

int x12_document_load(const char *path, x12_document_t *doc);
void x12_document_free(x12_document_t *doc);

#endif
