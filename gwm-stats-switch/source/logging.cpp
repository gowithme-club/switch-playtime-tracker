#include "logging.hpp"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <switch.h>
#include <sys/stat.h>
#include <time.h>

#include "paths.hpp"

static const char* LOG_FILE_PATH = GWM_CONFIG_DIR "/log.txt";

void initLog() {
    // The folder arrives with the archive, but a card upgraded from an older
    // build only has the legacy one — and a log that cannot be opened is the
    // one thing this module must not fail silently on.
    mkdir(GWM_CONFIG_DIR, 0777);
    FILE* f = fopen(LOG_FILE_PATH, "w");
    if (f) {
        fprintf(f, "--- gwm-stats-switch sysmodule started ---\n");
        fclose(f);
    }
}

void writeTrace(const char* stage) {
    FILE* f = fopen(LOG_FILE_PATH, "a");
    if (!f) return;

    const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    fprintf(f, "[+%llu.%03llu] %s\n", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000), stage);
    fclose(f);
}

void logHeapUsage(const char* where) {
    // uordblks/fordblks are what newlib has actually handed out and what is
    // still on its free lists. The gap between fordblks and the largest single
    // allocation that succeeds is fragmentation, which is why both the total and
    // the block count are worth printing.
    const struct mallinfo info = mallinfo();
    writeToLog("[Heap] %s: %u KB in use, %u KB free across %u blocks.", where,
               (unsigned)(info.uordblks / 1024), (unsigned)(info.fordblks / 1024),
               (unsigned)info.ordblks);
}

void writeToLog(const char* format, ...) {
    FILE* f = fopen(LOG_FILE_PATH, "a");
    if (!f) return;

    u64 timestamp;
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &timestamp))) {
        timestamp = time(NULL);
    }

    TimeLocationName location;
    timeGetDeviceLocationName(&location);
    static TimeZoneRule rule;  // 16KB, keep it off the tiny sysmodule stack
    timeLoadTimeZoneRule(&location, &rule);
    TimeCalendarTime t;
    timeToCalendarTime(&rule, timestamp, &t, nullptr);

    fprintf(f, "[%02d:%02d:%02d] ", t.hour, t.minute, t.second);

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}
