#pragma once

#include <stdint.h>

// Centralizes fan command supervision for manual, assist, startup, and auto modes.
class CommandSupervisor {
public:
    static constexpr uint32_t FAN_COMMAND_APPLY_TIMEOUT_MS = 3000;
    static constexpr uint32_t STARTUP_FAN_COMMAND_APPLY_TIMEOUT_MS = 60000;
    static constexpr uint32_t ASSIST_MAX_LIFETIME_MS = 300000;
    static constexpr uint32_t ASSIST_REARM_COOLDOWN_MS = 60000;
    static constexpr uint8_t OWNER_AUTO = 0;
    static constexpr uint8_t SOURCE_UNKNOWN = 0;

    enum CommandType : uint8_t {
        CMD_NONE = 0,
        CMD_MANUAL_FAN = 1,
        CMD_ASSIST_BOOST = 2,
        CMD_STARTUP_SPIN = 3,
        CMD_AUTO_CLEAR = 4,
    };

    enum CommandState : uint8_t {
        STATE_NONE = 0,
        STATE_ACCEPTED = 1,
        STATE_APPLIED = 2,
        STATE_VERIFIED = 3,
        STATE_EXPIRED = 4,
        STATE_FAILED = 5,
        STATE_ROLLED_BACK = 6,
        STATE_CLEARED = 7,
    };

    struct CommandRecord {
        uint32_t command_id;
        uint8_t command_type;
        uint8_t source;
        uint8_t owner;
        uint32_t requested_at_ms;
        uint32_t apply_deadline_ms;
        uint32_t expires_at_ms;
        uint8_t state;
        int16_t requested_value;
        int16_t effective_value;
        char failure_reason[32];
        char cleared_by[24];
    };

    struct ApplyToken {
        CommandRecord command;
        uint32_t target_revision;
        uint8_t target;
        bool has_command;
    };

    struct StatusSnapshot {
        uint8_t desired_owner;
        uint8_t effective_owner;
        uint8_t desired_source;
        uint8_t effective_source;
        uint8_t desired_speed;
        uint8_t effective_speed;
        bool has_active_command;
        CommandRecord active_command;
        CommandRecord last_command;
        char last_failsafe_reason[32];
        uint32_t pending_command_age_ms;
    };

    CommandSupervisor();

    void setEffectiveBaseline(uint8_t speed, uint8_t owner, uint8_t source);

    CommandRecord submitManualFan(uint8_t source, uint8_t owner, uint8_t speed, uint32_t now_ms);
    CommandRecord submitAssistBoost(uint8_t source, uint8_t owner, uint8_t speed,
                                           uint32_t hold_until_ms, uint32_t now_ms);
    CommandRecord submitStartupSpin(uint8_t source, uint8_t owner, uint8_t speed,
                                           uint32_t expires_at_ms, uint32_t now_ms);
    CommandRecord submitAutoClear(uint8_t source, uint8_t owner, uint32_t now_ms,
                                         const char* cleared_by);
    CommandRecord submitAutoClear(uint8_t source, uint32_t now_ms,
                                         const char* cleared_by) {
        return submitAutoClear(source, OWNER_AUTO, now_ms, cleared_by);
    }

    bool hasActiveFanCommand() const;
    bool hasFanCommand() const { return hasActiveFanCommand(); }
    bool currentCommand(CommandRecord* out) const;
    bool lastCommand(CommandRecord* out) const;
    CommandRecord fanCommand() const { return _fan_command; }
    CommandRecord lastCommand() const { return _last_command; }
    uint32_t pendingCommandAgeMs(uint32_t now_ms) const;
    uint32_t desiredCommandExpiryMs() const;

    uint8_t desiredOwner() const { return _desired_owner; }
    uint8_t effectiveOwner() const { return _effective_owner; }
    uint8_t desiredSource() const { return _desired_source; }
    uint8_t effectiveSource() const { return _effective_source; }
    uint8_t desiredSpeed() const { return _desired_speed; }
    uint8_t effectiveSpeed() const { return _effective_speed; }
    const char* lastFailsafeReason() const { return _last_failsafe_reason; }

    bool manualOverrideActive() const;
    bool manualOverrideDesired() const { return manualOverrideActive(); }
    bool manualActive() const { return manualOverrideActive(); }
    bool startupSpinActive() const;
    bool assistActive() const { return _assist_active; }
    uint32_t assistStartedMs() const { return _assist_started_ms; }
    uint32_t assistHoldUntilMs() const { return _assist_hold_until_ms; }
    uint32_t assistCooldownUntilMs() const { return _assist_cooldown_until_ms; }
    bool assistHardExpired(uint32_t now_ms) const;
    bool assistSoftExpired(uint32_t now_ms) const;
    bool assistCanTrigger(uint32_t now_ms, bool signal_present);

    ApplyToken applicationToken(uint8_t target, uint8_t source = SOURCE_UNKNOWN,
                                uint8_t owner = OWNER_AUTO, bool bind_command = true) const;
    bool applicationMatches(const ApplyToken& token) const;
    CommandRecord markFanApplied(const ApplyToken& token, uint8_t effective_speed, uint32_t now_ms);
    CommandRecord markFanVerified(const ApplyToken& token, uint8_t effective_speed, uint32_t now_ms);
    CommandRecord failFanApplication(const ApplyToken& token, const char* reason, uint32_t now_ms);
    void noteAutoControlApplied(const ApplyToken& token, uint8_t source, uint8_t speed, uint32_t now_ms);

    bool failPendingFan(const char* reason, uint32_t now_ms, CommandRecord* out = nullptr);
    CommandRecord failPendingFan(uint32_t now_ms, const char* reason) {
        failPendingFan(reason, now_ms, nullptr);
        return _last_command;
    }
    bool rollbackFanToAuto(const char* reason, uint32_t now_ms, CommandRecord* out = nullptr);
    CommandRecord rollbackFanToAuto(uint32_t now_ms, const char* reason) {
        rollbackFanToAuto(reason, now_ms, nullptr);
        return _last_command;
    }
    bool clearAssist(const char* cleared_by, uint32_t now_ms, CommandRecord* out = nullptr);
    CommandRecord clearAssist(uint32_t now_ms, const char* cleared_by) {
        clearAssist(cleared_by, now_ms, nullptr);
        return _last_command;
    }
    bool expireAssistHard(const char* reason, uint32_t now_ms, CommandRecord* out = nullptr);
    CommandRecord expireAssistHard(uint32_t now_ms, const char* reason) {
        expireAssistHard(reason, now_ms, nullptr);
        return _last_command;
    }
    bool clearActiveFanCommand(const char* cleared_by, uint32_t now_ms, CommandRecord* out = nullptr);
    bool checkFanApplyTimeout(uint32_t now_ms, CommandRecord* out = nullptr);

    void snapshot(uint32_t now_ms, StatusSnapshot* out) const;

    static const char* commandTypeName(uint8_t type);
    static const char* commandStateName(uint8_t state);

private:
    uint32_t _next_command_id;
    uint32_t _target_revision;
    bool _has_fan_command;
    CommandRecord _fan_command;
    bool _has_last_command;
    CommandRecord _last_command;
    uint8_t _desired_owner;
    uint8_t _effective_owner;
    uint8_t _desired_source;
    uint8_t _effective_source;
    uint8_t _desired_speed;
    uint8_t _effective_speed;
    char _last_failsafe_reason[32];
    bool _assist_active;
    uint32_t _assist_started_ms;
    uint32_t _assist_hold_until_ms;
    uint32_t _assist_cooldown_until_ms;
    bool _assist_cooldown_active;
    bool _assist_rearm_armed;

    void clearRecord(CommandRecord* record) const;
    void setFailureReason(CommandRecord* record, const char* reason) const;
    void setClearedBy(CommandRecord* record, const char* cleared_by) const;
    void noteLastCommand(const CommandRecord& record);
    CommandRecord acceptFanCommand(uint8_t type, uint8_t source, uint8_t owner,
                                          int16_t requested_value, uint32_t apply_deadline_ms,
                                          uint32_t expires_at_ms, uint32_t now_ms,
                                          const char* cleared_by);
    void finishFanCommand(uint8_t new_state, const char* failure_reason,
                          const char* cleared_by, uint32_t now_ms,
                          CommandRecord* out);
    void clearToAutoDesired(uint8_t source);
    void setDesired(uint8_t owner, uint8_t source, int16_t value);
    void setEffective(uint8_t owner, uint8_t source, int16_t value);
    void noteAssistCleared(uint32_t now_ms, bool enter_cooldown);
};
