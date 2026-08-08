#include "Ota.h"
#include <PsychicHttp.h>

namespace Ota {

void registerRoutes(PsychicHttpServer& server) {
  server.on("/api/update", HTTP_POST, [](PsychicRequest*, PsychicResponse* res) {
    return res->send(501, "text/plain", "OTA not implemented yet (build slice pending)");
  });
}

} // namespace Ota
