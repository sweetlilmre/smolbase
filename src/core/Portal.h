// Captive-portal DNS hijack: alive only in AP mode. The HTTP side (catch-all route,
// portal page) belongs to Web; this is just the wildcard DNS responder (async,
// runs on the lwIP tcpip thread — no task of our own).
#pragma once

namespace Portal {
void begin();
void end();
} // namespace Portal
