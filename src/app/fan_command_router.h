#pragma once

#include <stdint.h>
#include "auto_engine_v3.h"
#include "command_supervisor.h"

struct FanApplicationResult {
    CommandSupervisor::CommandRecord command;
    bool matched;
    bool sent;
    bool succeeded;
};
typedef bool (*FanSpeedWriter)(uint8_t speed, void* context);
// The token is captured before hardware work. The callback is the production
// PWM adapter on device and a controlled actuator at the host-test boundary.
FanApplicationResult applySupervisedFan(CommandSupervisor& supervisor,
    const CommandSupervisor::ApplyToken& token, bool needs_send, uint32_t now_ms,
    FanSpeedWriter writer, void* context);

enum class FanSelectionReason : uint8_t {
    Fallback = 0,
    Manual = 1,
    Startup = 2,
    Auto = 3,
    Schedule = 4,
};

struct FanSelection {
    uint8_t target_speed;
    FanSelectionReason reason;
};

FanSelection selectFanTarget(bool manual_override,
                             uint8_t manual_speed,
                             bool startup_active,
                             uint8_t startup_speed,
                             bool schedule_active,
                             uint8_t schedule_speed,
                             const AutoEngineV3::Result* v3,
                             uint8_t fallback_speed);

bool shouldSendFanCommand(bool module_ready,
                          bool actuation_ready,
                          uint8_t current_speed,
                          uint8_t target_speed,
                          uint32_t last_command_ms,
                          bool startup_special_case);
