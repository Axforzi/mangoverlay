#pragma once
// TEMPORARY instrumentation for the "HUD turns off on first multiplier change" bug.
// Appends one line per call to /tmp/mangohud-instrument.log
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cinttypes>

static void mangohud_dbg(const char* fmt, ...) {
    static std::uint64_t seq = 0;
    FILE* f = fopen("/tmp/mangohud-instrument.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%08" PRIu64 " t=%lld.%09ld] ", seq++, (long long)ts.tv_sec, ts.tv_nsec);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}