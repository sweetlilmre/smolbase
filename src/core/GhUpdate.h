// GitHub release OTA: version check and pull-and-flash from public releases.
// Routes: GET /api/update/check, POST /api/update/github.
// Decisions: wayfinder map #106 (tickets #107, #108, #109, #111).
#pragma once

class PsychicHttpServer;

namespace GhUpdate {
void registerRoutes(PsychicHttpServer& server);
} // namespace GhUpdate
