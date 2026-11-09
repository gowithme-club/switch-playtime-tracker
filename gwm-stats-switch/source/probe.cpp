#include "probe.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/filio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "logging.hpp"
#include "paths.hpp"
#include "version.hpp"

static const char* PROBE_MARKER_PATH = GWM_CONFIG_DIR "/probe.txt";

static IngestConfig g_probeConfig;
static Thread g_probeThread;

// Milliseconds since boot off the system tick. Deliberately not the `time`
// service: this module is debugging calls that hang, and a hung service must
// not be able to take the measurement of the hang with it.
static u64 nowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static void sleepMs(u64 ms) {
    svcSleepThread(ms * 1000000ULL);
}

// "http://192.168.1.70:3300" -> "192.168.1.70", 3300. A literal address only:
// resolving a name is a second failure mode, and the documented setup is an IP.
static bool splitEndpoint(const std::string& baseUrl, std::string* host, u16* port) {
    if (baseUrl.compare(0, 7, "http://") != 0) return false;

    const std::string rest = baseUrl.substr(7);
    const size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);

    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        *host = authority;
        *port = 80;
    } else {
        *host = authority.substr(0, colon);
        *port = (u16)atoi(authority.c_str() + colon + 1);
    }

    return !host->empty() && *port != 0;
}

static void probeThreadFunc(void*) {
    const IngestConfig& cfg = g_probeConfig;

    writeToLog("[Probe] --- transport probe: no blocking send, no blocking recv ---");

    std::string host;
    u16 port = 0;
    if (!splitEndpoint(cfg.baseUrl, &host, &port)) {
        writeToLog("[Probe] Skipped: '%s' is not a plain http:// endpoint.", cfg.baseUrl.c_str());
        return;
    }

    struct in_addr addr;
    addr.s_addr = inet_addr(host.c_str());
    if (addr.s_addr == INADDR_NONE) {
        writeToLog("[Probe] Skipped: '%s' is a name, not an address; the probe does no DNS.",
                   host.c_str());
        return;
    }

    writeTrace("probe: socket");
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        writeToLog("[Probe] socket() failed, errno %d.", errno);
        return;
    }

    struct sockaddr_in server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr = addr;

    // connect stays blocking: it is the one call that has never misbehaved here,
    // reaching the server in single-digit milliseconds every time.
    writeTrace("probe: connect (blocking)");
    u64 startMs = nowMs();
    const int connectResult = connect(fd, (struct sockaddr*)&server, sizeof(server));
    writeToLog("[Probe] connect(%s:%u) = %d, errno %d, %llu ms.", host.c_str(), (unsigned)port,
               connectResult, connectResult < 0 ? errno : 0, (unsigned long long)(nowMs() - startMs));
    if (connectResult < 0) {
        close(fd);
        return;
    }

    // Is the connection connect() reported actually established? A driver that
    // reports success optimistically would explain a send that waits forever.
    struct sockaddr_in peer = {};
    socklen_t peerLength = sizeof(peer);
    const int peerResult = getpeername(fd, (struct sockaddr*)&peer, &peerLength);
    int soError = -1;
    socklen_t soErrorLength = sizeof(soError);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soErrorLength);
    writeToLog("[Probe] getpeername = %d (%s:%u), SO_ERROR = %d.", peerResult,
               inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port), soError);

    // 0.3.9 asked for O_NONBLOCK through fcntl, was told "success", and then sat
    // in a blocking send anyway. ioctl(FIONBIO) is the other way to ask; both
    // answers are logged, and F_GETFL is read back rather than assumed.
    int one = 1;
    const int ioctlResult = ioctl(fd, FIONBIO, &one);
    const int flagsAfter = fcntl(fd, F_GETFL, 0);
    writeToLog("[Probe] ioctl(FIONBIO,1) = %d, errno %d; fcntl(F_GETFL) now 0x%x.", ioctlResult,
               ioctlResult < 0 ? errno : 0, (unsigned)flagsAfter);

    char request[512];
    const int requestLength = snprintf(
        request, sizeof(request),
        "POST /ingest/v1/heartbeat HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: %s probe\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}",
        host.c_str(), (unsigned)port, GWM_STATS_SWITCH_CLIENT, cfg.token.c_str());

    // MSG_DONTWAIT is passed straight through to the socket driver, so this one
    // call is non-blocking whatever the socket's own flags ended up being. If
    // this returns and 0.3.9's plain send did not, the socket flags are the bug.
    writeTrace("probe: send(MSG_DONTWAIT)");
    startMs = nowMs();
    int sent = 0;
    int sendRounds = 0;
    while (sent < requestLength && sendRounds < 60) {
        const ssize_t wrote = send(fd, request + sent, (size_t)(requestLength - sent), MSG_DONTWAIT);
        if (wrote > 0) {
            sent += (int)wrote;
            continue;
        }
        if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            writeToLog("[Probe] send() failed after %d/%d bytes, errno %d.", sent, requestLength, errno);
            break;
        }
        sendRounds++;
        sleepMs(50);
    }
    writeToLog("[Probe] sent %d/%d bytes in %llu ms (%d idle rounds).", sent, requestLength,
               (unsigned long long)(nowMs() - startMs), sendRounds);

    // Read until the server closes or three seconds pass. The status line is all
    // this needs: any complete response proves the exchange, and 401 proves it
    // as well as 200 does.
    writeTrace("probe: recv(MSG_DONTWAIT)");
    startMs = nowMs();
    std::string response;
    int recvRounds = 0;
    bool closed = false;
    while (recvRounds < 60 && response.size() < 2048) {
        char chunk[512];
        const ssize_t got = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (got > 0) {
            response.append(chunk, (size_t)got);
            continue;
        }
        if (got == 0) {
            closed = true;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            writeToLog("[Probe] recv() failed, errno %d.", errno);
            break;
        }
        recvRounds++;
        sleepMs(50);
    }

    const size_t lineEnd = response.find('\r');
    writeToLog("[Probe] got %u bytes in %llu ms (%d idle rounds, peer %s): %s", (unsigned)response.size(),
               (unsigned long long)(nowMs() - startMs), recvRounds, closed ? "closed" : "still open",
               response.empty() ? "(nothing)"
                                : response.substr(0, lineEnd == std::string::npos ? 60 : lineEnd).c_str());
    close(fd);

    // Last, and on a socket of its own: does poll come back when told to wait a
    // second? Nothing is ever readable on a connection that has sent nothing, so
    // the honest answer is "0, after about 1000 ms". This console has previously
    // answered both "immediately" and "never"; if it stops here, that is the
    // finding and everything above it is already in the file.
    writeTrace("probe: poll(POLLIN, 1000) on an idle socket");
    const int pollFd = socket(AF_INET, SOCK_STREAM, 0);
    if (pollFd < 0) {
        writeToLog("[Probe] second socket() failed, errno %d.", errno);
        return;
    }
    if (connect(pollFd, (struct sockaddr*)&server, sizeof(server)) < 0) {
        writeToLog("[Probe] second connect() failed, errno %d.", errno);
        close(pollFd);
        return;
    }

    struct pollfd waiter = {};
    waiter.fd = pollFd;
    waiter.events = POLLIN;

    startMs = nowMs();
    const int pollResult = poll(&waiter, 1, 1000);
    const u64 pollElapsed = nowMs() - startMs;
    writeToLog("[Probe] poll(1000 ms) returned %d after %llu ms, revents 0x%x, errno %d.", pollResult,
               (unsigned long long)pollElapsed, (unsigned)waiter.revents, pollResult < 0 ? errno : 0);
    close(pollFd);

    writeToLog("[Probe] --- probe complete ---");
}

void startTransportProbe(const IngestConfig& cfg) {
    FILE* marker = fopen(PROBE_MARKER_PATH, "r");
    if (marker == NULL) return;
    fclose(marker);

    // Consumed before the probe runs, not after: a probe that never returns must
    // not arm itself again on the next boot.
    remove(PROBE_MARKER_PATH);
    writeToLog("[Probe] Marker found and consumed; probing on a separate thread.");

    g_probeConfig = cfg;
    if (R_FAILED(threadCreate(&g_probeThread, probeThreadFunc, NULL, NULL, 0x4000, 0x2C, -2))) {
        writeToLog("[Probe] Could not start the probe thread.");
        return;
    }
    threadStart(&g_probeThread);
}
