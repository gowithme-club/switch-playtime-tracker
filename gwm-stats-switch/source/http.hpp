#pragma once

#include <switch.h>

#include <string>

/// Minimal HTTP/1.1 POST over a plain socket. There is no libcurl here on
/// purpose: curl brings a TLS stack, and both its code and its buffers are
/// memory a sysmodule holds for the life of the console — memory the system
/// then cannot give to an applet. The console crashed opening the album and a
/// game's privacy-policy browser until this went away.
///
/// `url` must be http://host[:port] (an IP is what the setup documents; a
/// hostname works when DNS does). `response` receives the raw body, framing and
/// all — the caller looks for what it needs rather than parsing HTTP properly.
/// Returns true only for a 2xx.
bool httpPost(const std::string& url, const char* path, const std::string& authToken, const char* body,
              std::string* response);
