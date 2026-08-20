// GitHub release OTA: version check and pull-and-flash from public releases.
// Routes: GET /api/update/check, POST /api/update/github.
// Decisions: wayfinder map #106 (tickets #107, #108, #109, #111).
#pragma once

class PsychicHttpServer;

namespace GhUpdate {
void registerRoutes(PsychicHttpServer& server);
// Call from the Arduino loop(). Runs the download state machine when an OTA
// was queued by the POST handler. Blocks the loop for the duration of the
// download — acceptable since the system OTA screen takes over during that time.
void tick();
} // namespace GhUpdate
