// PsychicHttp lifecycle and registration ordering. Order is structural (ticket #6):
// system API routes → app.registerRoutes() → static/catch-all LAST.
#pragma once

class App;
class PsychicHttpServer;

namespace Web {
void begin(App& app);
PsychicHttpServer& server(); // advanced/consumer escape hatch
} // namespace Web
