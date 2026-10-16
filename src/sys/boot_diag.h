#pragma once
#include "application.h"
#include <stddef.h>
#include <stdint.h>
#include "config_store.h"

// Boot-time diagnostics: why the device last reset and where config came
// from. Requires FEATURE_RESET_INFO, enabled from STARTUP() in main.cpp.
namespace BootDiag {

// Reset data the application watchdog passes to System.reset().
static constexpr uint32_t APP_WATCHDOG_RESET_DATA = 0x57444F47; // "WDOG"

void             capture(ConfigLoadResult config_load);
int              resetReason();
uint32_t         resetReasonData();
ConfigLoadResult configLoad();
const char*      resetReasonName(int reason);
// True when the application watchdog forced the last reset.
bool             appWatchdogReset();

// Short REBOOT event text, e.g. "boot r60 ok". Always NUL-terminated.
void formatEventMsg(char* out, size_t len);
// JSON for GET /api/debug/boot. Returns snprintf's result.
int  renderJson(char* out, size_t len, uint32_t uptime_s);

}  // namespace BootDiag
