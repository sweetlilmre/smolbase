#include "../core/App.h"
#include "../core/ConfigStore.h"
#include "../core/Display.h"
#include "../core/Secrets.h"
#include "CgmFetch.h"
#include "CgmKeys.h"
#include "GcmScreen.h"

class GcmApp : public App {
    GcmScreen screen;
    bool      parked = false;

public:
    void setup() override {
        static const SettingChoice kUnits[] = {{"mmol/L", "mmol"}, {"mg/dL", "mgdl"}};
        ConfigStore::setAppNote(
            "LibreLinkUp CGM display. "
            "Enter credentials at Settings → Secrets (llu_email, llu_pass).");
        ConfigStore::registerChoice(SettingSection::App, CgmKeys::UNIT,
                                    "Glucose unit", "mmol/L", "mmol", kUnits, 2);
        ConfigStore::registerInt(SettingSection::App, CgmKeys::INTERVAL,
                                 "Poll interval (min)", CgmKeys::DEF_INTERVAL, 1, 60);
        // Describe secrets so the stock Settings UI renders write-only inputs
        Secrets::describe(CgmKeys::EMAIL,    "LibreLinkUp email");
        Secrets::describe(CgmKeys::PASSWORD, "LibreLinkUp password",
                          "Your LibreView / LibreLinkUp account password");
        screen.begin();
        CgmFetch::begin();
        Display::setActive(&screen);
    }

    void loop() override {
        if (parked) return;
        CgmFetch::loop();
        if (const GcmData* d = CgmFetch::takeChanged()) {
            screen.data = *d;
            screen.markDirty();
        }
    }

    void onSystemEvent(SysEvent e) override {
        if (e == SysEvent::OtaStarting) { parked = true; return; }
        if (e == SysEvent::NetworkUp)       CgmFetch::forceRefresh();
        if (e == SysEvent::SettingsChanged) { CgmFetch::forceRefresh(); screen.markDirty(); }
    }
};

App& makeApp() {
    static GcmApp app;
    return app;
}
