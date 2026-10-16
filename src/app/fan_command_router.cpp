#include "fan_command_router.h"

FanApplicationResult applySupervisedFan(CommandSupervisor& supervisor,
    const CommandSupervisor::ApplyToken& token, bool needs_send, uint32_t now_ms,
    FanSpeedWriter writer, void* context) {
    FanApplicationResult result{};
    result.sent = needs_send;
    result.succeeded = !needs_send || (writer && writer(token.target, context));
    result.matched = supervisor.applicationMatches(token);
    if (!result.succeeded && result.matched && token.has_command &&
        token.command.command_type == CommandSupervisor::CMD_STARTUP_SPIN) {
        // Startup previously retried failed writes until its original apply
        // deadline. Keep that lifecycle and ID; stale failures still take the
        // token-attributed failure path below without touching a replacement.
        result.command = supervisor.fanCommand();
        return result;
    }
    result.command = result.succeeded
        ? supervisor.markFanVerified(token, token.target, now_ms)
        : supervisor.failFanApplication(token, "fan apply failed", now_ms);
    return result;
}

FanSelection selectFanTarget(bool manual_override,
                             uint8_t manual_speed,
                             bool startup_active,
                             uint8_t startup_speed,
                             bool schedule_active,
                             uint8_t schedule_speed,
                             const AutoEngineV3::Result* v3,
                             uint8_t fallback_speed) {
    if (manual_override) {
        return {manual_speed, FanSelectionReason::Manual};
    }
    if (startup_active) {
        return {startup_speed, FanSelectionReason::Startup};
    }
    if (schedule_active) {
        return {schedule_speed, FanSelectionReason::Schedule};
    }
    if (v3) {
        return {v3->quantized_step, FanSelectionReason::Auto};
    }
    return {fallback_speed, FanSelectionReason::Fallback};
}

bool shouldSendFanCommand(bool module_ready,
                          bool actuation_ready,
                          uint8_t current_speed,
                          uint8_t target_speed,
                          uint32_t last_command_ms,
                          bool startup_special_case) {
    if (!module_ready || !actuation_ready) return false;
    return current_speed == 0xFF ||
           target_speed != current_speed ||
           (last_command_ms == 0 && startup_special_case);
}
