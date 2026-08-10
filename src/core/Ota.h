// OTA update routes (firmware + filesystem). POST /api/update streams a
// multipart image into Update.h; GET /api/update/status reports progress.
#pragma once

class PsychicHttpServer;

namespace Ota {
void registerRoutes(PsychicHttpServer& server);

// Boot-loop guard (ticket #76). The device is OTA-only — no serial rescue —
// so a firmware that uploads fine but crashes at boot must not get to keep
// the boot slot. Ota.cpp overrides the arduino core's verifyRollbackLater()
// hook: a freshly flashed image boots in PENDING_VERIFY, and this tick
// confirms it (esp_ota_mark_app_valid_cancel_rollback) only after ~30 s of
// healthy uptime. Any crash/panic/WDT reset before that and the bootloader
// falls back to the previous app slot on its own. Call every main-loop pass.
void tickRollbackGuard();
} // namespace Ota
