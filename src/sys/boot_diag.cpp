#include "boot_diag.h"
#include <stdio.h>

namespace {
int              g_reset_reason = RESET_REASON_NONE;
uint32_t         g_reset_data = 0;
ConfigLoadResult g_config_load = CONFIG_LOAD_DEFAULTS;
}  // namespace

namespace BootDiag {

void capture(ConfigLoadResult config_load) {
    g_reset_reason = System.resetReason();
    g_reset_data = System.resetReasonData();
    g_config_load = config_load;
}

int resetReason() { return g_reset_reason; }
uint32_t resetReasonData() { return g_reset_data; }
ConfigLoadResult configLoad() { return g_config_load; }

const char* resetReasonName(int reason) {
    switch (reason) {
        case RESET_REASON_NONE:             return "none";
        case RESET_REASON_UNKNOWN:          return "unknown";
        case RESET_REASON_PIN_RESET:        return "pin_reset";
        case RESET_REASON_POWER_MANAGEMENT: return "power_management";
        case RESET_REASON_POWER_DOWN:       return "power_down";
        case RESET_REASON_POWER_BROWNOUT:   return "power_brownout";
        case RESET_REASON_WATCHDOG:         return "watchdog";
        case RESET_REASON_UPDATE:           return "update";
        case RESET_REASON_UPDATE_ERROR:     return "update_error";
        case RESET_REASON_UPDATE_TIMEOUT:   return "update_timeout";
        case RESET_REASON_FACTORY_RESET:    return "factory_reset";
        case RESET_REASON_SAFE_MODE:        return "safe_mode";
        case RESET_REASON_DFU_MODE:         return "dfu_mode";
        case RESET_REASON_PANIC:            return "panic";
        case RESET_REASON_USER:             return "user";
        default:                            return "other";
    }
}

bool appWatchdogReset() {
    return g_reset_reason == RESET_REASON_USER && g_reset_data == APP_WATCHDOG_RESET_DATA;
}

void formatEventMsg(char* out, size_t len) {
    if (!out || len == 0) return;
    if (appWatchdogReset()) {
        snprintf(out, len, "boot rwd %s", configLoadResultName(g_config_load));
    } else {
        snprintf(out, len, "boot r%d %s", g_reset_reason, configLoadResultName(g_config_load));
    }
}

int renderJson(char* out, size_t len, uint32_t uptime_s) {
    return snprintf(out, len,
                    "{\"reset_reason\":%d,\"reset_reason_name\":\"%s\","
                    "\"reset_data\":%lu,\"config_load\":\"%s\",\"uptime_s\":%lu}",
                    g_reset_reason,
                    appWatchdogReset() ? "app_watchdog" : resetReasonName(g_reset_reason),
                    (unsigned long)g_reset_data, configLoadResultName(g_config_load),
                    (unsigned long)uptime_s);
}

}  // namespace BootDiag
