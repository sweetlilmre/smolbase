// OTA update routes (firmware + filesystem). POST /api/update streams a
// multipart image into Update.h; GET /api/update/status reports progress.
#pragma once

class PsychicHttpServer;

namespace Ota {
void registerRoutes(PsychicHttpServer& server);
} // namespace Ota
