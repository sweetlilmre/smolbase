// GitHub release OTA: version check and pull-and-flash from public releases.
// Routes: GET /api/update/check, GET /api/update/ghprogress, POST /api/update/github.
// The POST handler spawns a Core 0 download task; nothing runs in loop().
// Decisions: wayfinder maps #106 (feature) and #112 (memory optimization).
#pragma once

class PsychicHttpServer;

namespace GhUpdate {
void registerRoutes(PsychicHttpServer& server);
} // namespace GhUpdate
