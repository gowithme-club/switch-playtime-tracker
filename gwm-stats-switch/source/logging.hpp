#pragma once

// Truncates the log file and writes the startup banner.
void initLog();

// Appends a formatted, timestamped line to the log file.
void writeToLog(const char* format, ...);

// How much of the module's heap is in use and how much is left, at a named
// point. Every memory decision in this module has so far been made by argument
// rather than measurement, and it has cost: the socket driver was starved into
// never sending a byte, and the play-sessions POST failed with curl unable to
// allocate. This is the number those arguments were missing.
void logHeapUsage(const char* where);

// A breadcrumb dropped immediately before a call that could block, so that a
// module which stops responding still says which call it stopped in — the last
// line in the file is the one that never returned.
//
// It deliberately avoids everything writeToLog needs: no `time` service and no
// timezone rule, only the system tick, because those services are themselves
// candidates for the thing that hangs. The stamp is seconds since boot.
void writeTrace(const char* stage);
