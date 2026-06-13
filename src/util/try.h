#ifndef SCRIBE_TRY_H
#define SCRIBE_TRY_H

/* Evaluate expr (an int rc) and return it from the enclosing function if it
 * is not X12_OK. The caller must have X12_OK in scope (x12_parser.h). */
#define TRY(expr)           \
    do                      \
    {                       \
        int rc__ = (expr);  \
        if (rc__ != X12_OK) \
        {                   \
            return rc__;    \
        }                   \
    } while (0)

#endif
