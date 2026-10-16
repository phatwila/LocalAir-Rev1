#include "watchdog.h"
#include "boot_diag.h"

ApplicationWatchdog* Watchdog::_wd = nullptr;

// Tag the reset so boot diagnostics can tell a watchdog hang from a
// requested reboot. Both report RESET_REASON_USER.
static void watchdogReset() { System.reset(BootDiag::APP_WATCHDOG_RESET_DATA); }

void Watchdog::begin() {
    _wd = new ApplicationWatchdog(TIMEOUT_MS, watchdogReset);
}

void Watchdog::feed() {
    if (_wd) _wd->checkin();
}
