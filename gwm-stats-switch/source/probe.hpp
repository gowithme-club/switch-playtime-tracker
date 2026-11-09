#pragma once

#include "config.hpp"

// A one-shot diagnostic for the bug that has outlasted four explanations: the
// module connects to the ingest server and then never sends a byte, and curl's
// own CURLOPT_TIMEOUT never fires — which it cannot do while it is stuck inside
// a call that does not return.
//
// 0.3.9 found the call. A single send() of 300 bytes into an established
// connection did not return in 63 seconds, on a socket whose fcntl(F_SETFL,
// O_NONBLOCK) had just reported success. So the socket was still blocking and
// send blocked in it, which is precisely how curl hangs: curl asks for a
// non-blocking socket through fcntl too.
//
// What this build measures, in order: whether the connection connect() reports
// is real, whether ioctl(FIONBIO) sets what fcntl could not, and whether
// send(MSG_DONTWAIT) — a flag the driver reads directly, ignoring the socket's
// own — comes back. Nothing here issues a blocking send or recv again.
//
// Two rules keep the diagnostic from costing more than it tells:
//
//   * It runs on its own thread. In 0.3.9 it ran on the main one, before the
//     module had registered with psc, so when it wedged the module could not be
//     switched off from Sysmodules and the console had to be rebooted.
//   * It runs only when sdmc:/config/gwm-stats-switch/probe.txt exists, and it
//     deletes that file before it starts. A wedged probe therefore cannot come
//     back on the next boot on its own.
void startTransportProbe(const IngestConfig& cfg);
