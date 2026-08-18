#pragma once
#include "GcmData.h"

namespace CgmFetch {
    void begin();                   // creates the FreeRTOS fetch task; call from setup()
    void loop();                    // schedule / promote fetches; call from App::loop()
    const GcmData* takeChanged();   // &current if data changed this pass, else nullptr
    void forceRefresh();            // arm for immediate fetch on the next loop() pass
}
