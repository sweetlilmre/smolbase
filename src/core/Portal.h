// Captive-portal DNS hijack: alive only in AP mode. The HTTP side (catch-all route,
// portal page) belongs to Web; this is just the wildcard DNS pump on its own task.
#pragma once

namespace Portal {
void begin(); // start DNS server + pump task (core 0)
void end();
} // namespace Portal
