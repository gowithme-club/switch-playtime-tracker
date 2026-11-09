#include "http.hpp"

#include <curl/curl.h>
#include <string.h>
#include <strings.h>

#include <string>

#include "logging.hpp"
#include "utilities.hpp"
#include "version.hpp"

// curl narrates its own progress — which host it is trying, when the connection
// is up, when the request is away. When a request hangs with no error, that
// narration is the only thing that says which of those steps it hung on.
static void traceCurlLines(const char* prefix, const char* data, size_t size) {
    size_t start = 0;

    while (start < size) {
        size_t end = start;
        while (end < size && data[end] != '\n' && data[end] != '\r') end++;

        if (end > start) {
            char line[128];
            size_t length = end - start;
            if (length > sizeof(line) - 1) length = sizeof(line) - 1;
            memcpy(line, data + start, length);
            line[length] = '\0';

            char stage[160];
            snprintf(stage, sizeof(stage), "      %s %s", prefix, line);
            writeTrace(stage);
        }

        start = end;
        while (start < size && (data[start] == '\n' || data[start] == '\r')) start++;
    }
}

static int debugCallback(CURL* handle, curl_infotype type, char* data, size_t size, void* context) {
    (void)handle;
    (void)context;

    switch (type) {
        case CURLINFO_TEXT:
            traceCurlLines("curl:", data, size);
            break;
        // Request headers are logged when curl hands them to the socket and the
        // response headers when the first byte comes back, which is what tells
        // a stalled send apart from a server that never answers.
        case CURLINFO_HEADER_OUT:
            traceCurlLines("curl >", data, size);
            break;
        case CURLINFO_HEADER_IN:
            traceCurlLines("curl <", data, size);
            break;
        default:
            break;
    }

    return 0;
}

static size_t writeCallback(char* data, size_t size, size_t count, void* context) {
    const size_t length = size * count;
    if (context != NULL) static_cast<std::string*>(context)->append(data, length);
    return length;
}

static size_t headerCallback(char* data, size_t size, size_t count, void* context) {
    (void)context;
    const size_t length = size * count;
    std::string header(data, length);

    if (header.size() >= 5 && strncasecmp(header.c_str(), "Date:", 5) == 0) {
        const char* value = header.c_str() + 5;
        while (*value == ' ' || *value == '\t') value++;

        const time_t serverUtc = curl_getdate(value, NULL);
        if (serverUtc != -1) {
            const u64 rawLocal = getRawNowSec();
            if (rawLocal > 0) updateTimeOffset((int64_t)serverUtc - (int64_t)rawLocal);
        }
    }

    return length;
}

bool httpPost(const std::string& url, const char* path, const std::string& authToken, const char* body,
              std::string* response) {
    if (url.compare(0, 7, "http://") != 0) {
        writeToLog("[Http] Only plain http:// is supported.");
        return false;
    }

    writeTrace("    http: curl_easy_init");
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        writeToLog("[Http] curl_easy_init failed (heap pressure).");
        logHeapUsage("at curl_easy_init failure");
        return false;
    }

    const std::string target = url + path;
    const std::string authorization = "Authorization: Bearer " + authToken;
    // The User-Agent names the client for HTTP logs, where nothing else does.
    // X-Client-Version is the bare version: the server already knows the
    // device's platform from its token and shows this next to the device's
    // label on its status page, so a name there would repeat the platform.
    const std::string userAgent = GWM_STATS_SWITCH_CLIENT;
    const std::string clientVersion = std::string("X-Client-Version: ") + GWM_STATS_SWITCH_VERSION;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, clientVersion.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, authorization.c_str());
    headers = curl_slist_append(headers, "Connection: close");

    if (response != NULL) response->clear();

    curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // curl's own buffers, sized for a general-purpose client. Left alone it asks
    // for a 64 KB upload buffer and a 16 KB receive buffer in single contiguous
    // pieces, and on this heap that is the difference between a request going
    // out and curl_easy_perform returning CURLE_OUT_OF_MEMORY — which is exactly
    // how the play-sessions POST failed with 93 KB free, but free in eight
    // fragments. 16 KB is curl's floor for the upload buffer. Neither is a
    // constraint on the traffic: the largest request this module sends is a
    // handful of play events and no response has reached a kilobyte.
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 16384L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debugCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
#ifdef CURLOPT_PROTOCOLS_STR
    // Do not initialize or negotiate the TLS stack. This module only talks to
    // the configured LAN endpoint and keeps the rest of curl out of the hot path.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP);
#endif

    // DNS needs the service manager, and this module closes it.
    //
    // libnx resolves names through sfdnsres, and sfdnsres has no init function:
    // every single request opens the service for itself, with
    // smGetServiceOriginal (libnx nx/source/services/sfdnsres.c). That needs a
    // live sm session, and __appInit ends with smExit — every other service
    // this module uses was opened while sm was still up. So a hostname in the
    // config fails with "Couldn't resolve host name" no matter what the console
    // or its DNS server does, while an IP address works, because an IP never
    // reaches the resolver.
    //
    // sm is therefore held for the length of the request and given back, the
    // same way acc and pdm:qry are (see openUserServices). Both calls are
    // reference counted, so this nests safely.
    const bool smHeld = R_SUCCEEDED(smInitialize());
    if (!smHeld) writeToLog("[Http] smInitialize failed; a hostname will not resolve.");

    writeTrace("    http: curl_easy_perform");
    const CURLcode result = curl_easy_perform(curl);
    writeTrace("    http: perform returned");

    if (smHeld) smExit();
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        writeToLog("[Http] POST %s failed: %s", path, curl_easy_strerror(result));
        if (result == CURLE_OUT_OF_MEMORY) logHeapUsage("at CURLE_OUT_OF_MEMORY");
        return false;
    }

    if (status < 200 || status >= 300) {
        writeToLog("[Http] POST %s returned HTTP %ld.", path, status);
        return false;
    }

    static bool announced = false;
    if (!announced) {
        announced = true;
        writeToLog("[Http] Transport works through curl (%s).", userAgent.c_str());
    }
    return true;
}
