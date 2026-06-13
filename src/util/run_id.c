#include "run_id.h"

#include "x12_parser.h"

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

int scribe_run_id_generate(char *out, size_t out_len)
{
    static unsigned long counter = 0u;
    time_t now;
    clock_t ticks;
    long pid;
    int written;

    if (out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    now = time(NULL);
    ticks = clock();
#ifdef _WIN32
    pid = (long)_getpid();
#else
    pid = (long)getpid();
#endif
    counter++;

    written = snprintf(
        out,
        out_len,
        "run-%lld-%ld-%lu-%lu",
        (long long)now,
        pid,
        (unsigned long)ticks,
        counter);
    if (written < 0 || (size_t)written >= out_len)
    {
        out[0] = '\0';
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}
