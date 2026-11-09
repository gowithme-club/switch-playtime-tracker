#pragma once

#include "config.hpp"

/// Drops an Ultrahand API toast via a `.notify` JSON file. No-op when the
/// config flag is off, Ultrahand is missing, or the write fails — the module
/// must not care whether a toast appeared.
void notifyUltrahand(const IngestConfig& cfg, const char* text);
