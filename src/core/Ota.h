// OTA update routes (firmware + filesystem). Scaffold: route placeholder only —
// the real upload handler is the "Build: OTA slice" ticket.
#pragma once

class PsychicHttpServer;

namespace Ota {
void registerRoutes(PsychicHttpServer& server);
} // namespace Ota
