#pragma once
namespace CgmKeys {
    // Secrets (Secrets::get/set — never registered settings, never served)
    constexpr const char* EMAIL    = "llu_email";
    constexpr const char* PASSWORD = "llu_pass";
    // Settings (registered in GcmApp::setup())
    constexpr const char* UNIT     = "cgm_unit";
    constexpr const char* INTERVAL = "cgm_interval";
    constexpr const char* DEF_UNIT = "mmol";
    constexpr int         DEF_INTERVAL = 5; // minutes
}
