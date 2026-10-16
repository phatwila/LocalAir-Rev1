#pragma once
#include "application.h"

// Hardware watchdog wrapper.
// Particle Device OS exposes the IWDG via ApplicationWatchdog.
// 60s timeout — if loop() hangs, device reboots.
class Watchdog {
public:
    static constexpr uint32_t TIMEOUT_MS = 60000;

    static void begin();
    static void feed();  // call at least once per TIMEOUT_MS

private:
    static ApplicationWatchdog* _wd;
};
