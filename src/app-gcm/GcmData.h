#pragma once
#include <cstdint>

struct GcmData {
    char    name[32];
    float   glucose;
    uint8_t trendArrow; // 1=↓↓ 2=↓ 3=→ 4=↑ 5=↑↑; 0=unknown
    int8_t  inRange;    // 1=in range (green), 0=out of range (red), -1=unknown
    float   spark[24];
    uint8_t sparkCount;
    bool    valid;
    bool    error;
    bool    loginError;      // auth failed — distinct from transient fetch errors
    bool    noCredentials;   // secrets not yet entered — show setup prompt
    bool    mgdl;
    uint32_t lastOkMs;

    void clear() {
        name[0]     = 0;
        glucose     = 0;
        trendArrow  = 0;
        inRange     = -1;
        sparkCount  = 0;
        valid       = false;
        error       = false;
        loginError    = false;
        noCredentials = false;
        mgdl          = false;
        lastOkMs    = 0;
    }
};
