#include "command_supervisor.h"
#include "../algo/fan_speed.h"

#include <string.h>

namespace {

static void safeCopy(char* out, size_t out_len, const char* value, const char* fallback) {
    if (!out || out_len == 0) return;
    const char* src = (value && value[0]) ? value : fallback;
    if (!src) src = "";
    strncpy(out, src, out_len - 1);
    out[out_len - 1] = '\0';
}

}  // namespace

CommandSupervisor::CommandSupervisor()
    : _next_command_id(1),
      _target_revision(0),
      _has_fan_command(false),
      _has_last_command(false),
      _desired_owner(OWNER_AUTO),
      _effective_owner(OWNER_AUTO),
      _desired_source(SOURCE_UNKNOWN),
      _effective_source(SOURCE_UNKNOWN),
      _desired_speed(0),
      _effective_speed(0),
      _assist_active(false),
      _assist_started_ms(0),
      _assist_hold_until_ms(0),
      _assist_cooldown_until_ms(0),
      _assist_cooldown_active(false),
      _assist_rearm_armed(true) {
    clearRecord(&_fan_command);
    clearRecord(&_last_command);
    safeCopy(_last_failsafe_reason, sizeof(_last_failsafe_reason), "none", "none");
}

void CommandSupervisor::clearRecord(CommandRecord* record) const {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    safeCopy(record->failure_reason, sizeof(record->failure_reason), "none", "none");
    safeCopy(record->cleared_by, sizeof(record->cleared_by), "none", "none");
}

void CommandSupervisor::setFailureReason(CommandRecord* record, const char* reason) const {
    if (!record) return;
    safeCopy(record->failure_reason, sizeof(record->failure_reason), reason, "none");
}

void CommandSupervisor::setClearedBy(CommandRecord* record, const char* cleared_by) const {
    if (!record) return;
    safeCopy(record->cleared_by, sizeof(record->cleared_by), cleared_by, "none");
}

void CommandSupervisor::noteLastCommand(const CommandRecord& record) {
    _last_command = record;
    _has_last_command = true;
}

void CommandSupervisor::setDesired(uint8_t owner, uint8_t source, int16_t value) {
    _desired_owner = owner;
    _desired_source = source;
    _desired_speed = value > 0 ? (uint8_t)value : 0;
}

void CommandSupervisor::setEffective(uint8_t owner, uint8_t source, int16_t value) {
    _effective_owner = owner;
    _effective_source = source;
    _effective_speed = value > 0 ? (uint8_t)value : 0;
}

void CommandSupervisor::clearToAutoDesired(uint8_t source) {
    _desired_owner = OWNER_AUTO;
    _desired_source = source;
    _desired_speed = 0;
}

void CommandSupervisor::noteAssistCleared(uint32_t now_ms, bool enter_cooldown) {
    _assist_active = false;
    _assist_started_ms = 0;
    _assist_hold_until_ms = 0;
    if (enter_cooldown) {
        _assist_cooldown_until_ms = now_ms + ASSIST_REARM_COOLDOWN_MS;
        _assist_cooldown_active = true;
        _assist_rearm_armed = false;
    } else if (!_assist_cooldown_active || (int32_t)(now_ms - _assist_cooldown_until_ms) >= 0) {
        _assist_cooldown_active = false;
        _assist_rearm_armed = true;
    }
}

void CommandSupervisor::setEffectiveBaseline(uint8_t speed, uint8_t owner, uint8_t source) {
    setEffective(owner, source, speed);
    if (!_has_fan_command) {
        setDesired(owner, source, speed);
    }
}

CommandSupervisor::CommandRecord CommandSupervisor::acceptFanCommand(
    uint8_t type, uint8_t source, uint8_t owner, int16_t requested_value,
    uint32_t apply_deadline_ms, uint32_t expires_at_ms, uint32_t now_ms,
    const char* cleared_by) {
    if (_has_fan_command) {
        CommandRecord replaced = _fan_command;
        replaced.state = STATE_CLEARED;
        setClearedBy(&replaced, cleared_by && cleared_by[0] ? cleared_by : "superseded");
        noteLastCommand(replaced);
    }
    clearRecord(&_fan_command);
    if (_next_command_id == 0) ++_next_command_id;
    _fan_command.command_id = _next_command_id++;
    ++_target_revision;
    _fan_command.command_type = type;
    _fan_command.source = source;
    _fan_command.owner = owner;
    _fan_command.requested_at_ms = now_ms;
    _fan_command.apply_deadline_ms = now_ms + apply_deadline_ms;
    _fan_command.expires_at_ms = expires_at_ms;
    _fan_command.state = STATE_ACCEPTED;
    _fan_command.requested_value = requested_value;
    _fan_command.effective_value = _effective_speed;
    _has_fan_command = true;
    setDesired(owner, source, requested_value);
    noteLastCommand(_fan_command);
    return _fan_command;
}

CommandSupervisor::CommandRecord CommandSupervisor::submitManualFan(
    uint8_t source, uint8_t owner, uint8_t speed, uint32_t now_ms) {
    return acceptFanCommand(CMD_MANUAL_FAN, source, owner, speed,
                            FAN_COMMAND_APPLY_TIMEOUT_MS, 0, now_ms, "manual_override");
}

CommandSupervisor::CommandRecord CommandSupervisor::submitAssistBoost(
    uint8_t source, uint8_t owner, uint8_t speed, uint32_t hold_until_ms,
    uint32_t now_ms) {
    if (_assist_active && _has_fan_command && _fan_command.command_type == CMD_ASSIST_BOOST) {
        _assist_hold_until_ms = hold_until_ms;
        _assist_rearm_armed = false;
        if (_fan_command.requested_value == 0 ||
            fanFasterThan(speed, (uint8_t)_fan_command.requested_value)) {
            _fan_command.requested_value = speed;
            ++_target_revision;
            _fan_command.state = STATE_ACCEPTED;
            _fan_command.apply_deadline_ms = now_ms + FAN_COMMAND_APPLY_TIMEOUT_MS;
        }
        if (_desired_speed == 0 || fanFasterThan(speed, _desired_speed)) {
            _desired_speed = speed;
        }
        _fan_command.expires_at_ms = _assist_started_ms + ASSIST_MAX_LIFETIME_MS;
        noteLastCommand(_fan_command);
        return _fan_command;
    }
    _assist_active = true;
    _assist_started_ms = now_ms;
    _assist_hold_until_ms = hold_until_ms;
    _assist_rearm_armed = false;
    return acceptFanCommand(CMD_ASSIST_BOOST, source, owner, speed,
                            FAN_COMMAND_APPLY_TIMEOUT_MS,
                            now_ms + ASSIST_MAX_LIFETIME_MS, now_ms, "assist_rearm");
}

CommandSupervisor::CommandRecord CommandSupervisor::submitStartupSpin(
    uint8_t source, uint8_t owner, uint8_t speed, uint32_t expires_at_ms,
    uint32_t now_ms) {
    return acceptFanCommand(CMD_STARTUP_SPIN, source, owner, speed,
                            STARTUP_FAN_COMMAND_APPLY_TIMEOUT_MS, expires_at_ms, now_ms,
                            "startup_spin");
}

CommandSupervisor::CommandRecord CommandSupervisor::submitAutoClear(
    uint8_t source, uint8_t owner, uint32_t now_ms, const char* cleared_by) {
    return acceptFanCommand(CMD_AUTO_CLEAR, source, owner, 0,
                            FAN_COMMAND_APPLY_TIMEOUT_MS, 0, now_ms, cleared_by);
}

bool CommandSupervisor::hasActiveFanCommand() const {
    return _has_fan_command;
}

bool CommandSupervisor::currentCommand(CommandRecord* out) const {
    if (!out) return false;
    if (_has_fan_command) {
        *out = _fan_command;
        return true;
    }
    clearRecord(out);
    return false;
}

bool CommandSupervisor::lastCommand(CommandRecord* out) const {
    if (!out || !_has_last_command) return false;
    *out = _last_command;
    return true;
}

uint32_t CommandSupervisor::pendingCommandAgeMs(uint32_t now_ms) const {
    CommandRecord current{};
    if (!currentCommand(&current)) return 0;
    return now_ms - current.requested_at_ms;
}

uint32_t CommandSupervisor::desiredCommandExpiryMs() const {
    return _has_fan_command ? _fan_command.expires_at_ms : 0;
}

bool CommandSupervisor::manualOverrideActive() const {
    return _desired_owner == 1 && _has_fan_command && _fan_command.command_type == CMD_MANUAL_FAN;
}

bool CommandSupervisor::startupSpinActive() const {
    return _has_fan_command && _fan_command.command_type == CMD_STARTUP_SPIN &&
           (_fan_command.state == STATE_ACCEPTED || _fan_command.state == STATE_APPLIED ||
            _fan_command.state == STATE_VERIFIED);
}

bool CommandSupervisor::assistHardExpired(uint32_t now_ms) const {
    return _assist_active && (now_ms - _assist_started_ms) >= ASSIST_MAX_LIFETIME_MS;
}

bool CommandSupervisor::assistSoftExpired(uint32_t now_ms) const {
    return _assist_active && (int32_t)(now_ms - _assist_hold_until_ms) >= 0;
}

bool CommandSupervisor::assistCanTrigger(uint32_t now_ms, bool signal_present) {
    if (_assist_active) return signal_present;
    if (_assist_cooldown_active && (int32_t)(now_ms - _assist_cooldown_until_ms) >= 0)
        _assist_cooldown_active = false;
    if (!signal_present) {
        if (!_assist_cooldown_active) {
            _assist_rearm_armed = true;
        }
        return false;
    }
    if (_assist_cooldown_active) return false;
    return _assist_rearm_armed;
}

CommandSupervisor::ApplyToken CommandSupervisor::applicationToken(
    uint8_t target, uint8_t source, uint8_t owner, bool bind_command) const {
    ApplyToken token{};
    token.has_command = _has_fan_command && bind_command;
    token.target_revision = _target_revision;
    token.target = target;
    if (token.has_command) token.command = _fan_command;
    else {
        clearRecord(&token.command);
        token.command.source = source;
        token.command.owner = owner;
        token.command.requested_value = target;
    }
    return token;
}

bool CommandSupervisor::applicationMatches(const ApplyToken& token) const {
    if (token.target_revision != _target_revision || token.has_command != _has_fan_command) return false;
    return !token.has_command || token.command.command_id == _fan_command.command_id;
}

CommandSupervisor::CommandRecord CommandSupervisor::markFanApplied(
    const ApplyToken& token, uint8_t effective_speed, uint32_t) {
    CommandRecord result = token.command;
    result.state = STATE_APPLIED;
    result.effective_value = effective_speed;
    if (token.has_command && applicationMatches(token)) {
        _fan_command.state = result.state;
        _fan_command.effective_value = effective_speed;
        noteLastCommand(_fan_command);
        result = _fan_command;
    }
    return result;
}

CommandSupervisor::CommandRecord CommandSupervisor::markFanVerified(
    const ApplyToken& token, uint8_t effective_speed, uint32_t /*now_ms*/) {
    CommandRecord result = token.command;
    result.state = STATE_VERIFIED;
    result.effective_value = effective_speed;
    // Physical application belongs to its captured owner, even if superseded.
    // Never use a replacement command's metadata to describe this result.
    setEffective(token.command.owner, token.command.source, effective_speed);
    if (!applicationMatches(token)) return result;
    if (!token.has_command) {
        if (_desired_owner == OWNER_AUTO) setDesired(OWNER_AUTO, token.command.source, effective_speed);
        return result;
    }
    _fan_command.state = STATE_VERIFIED;
    _fan_command.effective_value = effective_speed;
    if (_fan_command.command_type == CMD_AUTO_CLEAR) {
        setEffective(OWNER_AUTO, _fan_command.source, effective_speed);
        clearToAutoDesired(_fan_command.source);
        noteLastCommand(_fan_command);
        _has_fan_command = false;
        clearRecord(&_fan_command);
        return _last_command;
    }
    if (_fan_command.command_type == CMD_ASSIST_BOOST) {
        _assist_active = true;
    }
    noteLastCommand(_fan_command);
    return _fan_command;
}

CommandSupervisor::CommandRecord CommandSupervisor::failFanApplication(
    const ApplyToken& token, const char* reason, uint32_t now_ms) {
    CommandRecord result = token.command;
    result.state = STATE_FAILED;
    result.effective_value = _effective_speed;
    setFailureReason(&result, reason);
    if (token.has_command && applicationMatches(token)) {
        finishFanCommand(STATE_FAILED, reason, "apply_failed", now_ms, &result);
    }
    return result;
}

void CommandSupervisor::noteAutoControlApplied(
    const ApplyToken& token, uint8_t source, uint8_t speed, uint32_t now_ms) {
    ApplyToken applied = token;
    if (!applied.has_command) {
        applied.command.owner = OWNER_AUTO;
        applied.command.source = source;
    }
    markFanVerified(applied, speed, now_ms);
}

void CommandSupervisor::finishFanCommand(uint8_t new_state, const char* failure_reason,
                                         const char* cleared_by, uint32_t now_ms,
                                         CommandRecord* out) {
    _fan_command.state = new_state;
    _fan_command.effective_value = _effective_speed;
    setFailureReason(&_fan_command, failure_reason);
    setClearedBy(&_fan_command, cleared_by);
    noteLastCommand(_fan_command);
    if (new_state == STATE_FAILED || new_state == STATE_ROLLED_BACK || new_state == STATE_EXPIRED) {
        safeCopy(_last_failsafe_reason, sizeof(_last_failsafe_reason), failure_reason, "failed");
    }
    if (_fan_command.command_type == CMD_ASSIST_BOOST) {
        noteAssistCleared(now_ms, new_state == STATE_EXPIRED ||
                                   new_state == STATE_FAILED ||
                                   new_state == STATE_ROLLED_BACK);
    }
    clearToAutoDesired(_fan_command.source);
    _has_fan_command = false;
    if (out) *out = _last_command;
    clearRecord(&_fan_command);
}

bool CommandSupervisor::failPendingFan(const char* reason, uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command) return false;
    if (!(_fan_command.state == STATE_ACCEPTED || _fan_command.state == STATE_APPLIED)) return false;
    finishFanCommand(STATE_FAILED, reason, "apply_timeout", now_ms, out);
    return true;
}

bool CommandSupervisor::rollbackFanToAuto(const char* reason, uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command) return false;
    finishFanCommand(STATE_ROLLED_BACK, reason, "auto_fallback", now_ms, out);
    return true;
}

bool CommandSupervisor::clearAssist(const char* cleared_by, uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command || _fan_command.command_type != CMD_ASSIST_BOOST) return false;
    finishFanCommand(STATE_CLEARED, "none", cleared_by, now_ms, out);
    return true;
}

bool CommandSupervisor::expireAssistHard(const char* reason, uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command || _fan_command.command_type != CMD_ASSIST_BOOST) return false;
    finishFanCommand(STATE_EXPIRED, reason, "assist_max_lifetime", now_ms, out);
    return true;
}

bool CommandSupervisor::clearActiveFanCommand(const char* cleared_by, uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command) return false;
    finishFanCommand(STATE_CLEARED, "none", cleared_by, now_ms, out);
    return true;
}

bool CommandSupervisor::checkFanApplyTimeout(uint32_t now_ms, CommandRecord* out) {
    if (!_has_fan_command) return false;
    if (!(_fan_command.state == STATE_ACCEPTED || _fan_command.state == STATE_APPLIED)) return false;
    if ((int32_t)(now_ms - _fan_command.apply_deadline_ms) < 0) return false;
    return failPendingFan("apply timeout", now_ms, out);
}

void CommandSupervisor::snapshot(uint32_t now_ms, StatusSnapshot* out) const {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->desired_owner = _desired_owner;
    out->effective_owner = _effective_owner;
    out->desired_source = _desired_source;
    out->effective_source = _effective_source;
    out->desired_speed = _desired_speed;
    out->effective_speed = _effective_speed;
    out->has_active_command = _has_fan_command;
    if (_has_fan_command) out->active_command = _fan_command;
    if (_has_last_command) out->last_command = _last_command;
    safeCopy(out->last_failsafe_reason, sizeof(out->last_failsafe_reason),
             _last_failsafe_reason, "none");
    out->pending_command_age_ms = pendingCommandAgeMs(now_ms);
}

const char* CommandSupervisor::commandTypeName(uint8_t type) {
    switch (type) {
        case CMD_MANUAL_FAN: return "manual_fan";
        case CMD_ASSIST_BOOST: return "assist_boost";
        case CMD_STARTUP_SPIN: return "startup_spin";
        case CMD_AUTO_CLEAR: return "auto_clear";
        default: return "none";
    }
}

const char* CommandSupervisor::commandStateName(uint8_t state) {
    switch (state) {
        case STATE_ACCEPTED: return "accepted";
        case STATE_APPLIED: return "applied";
        case STATE_VERIFIED: return "verified";
        case STATE_EXPIRED: return "expired";
        case STATE_FAILED: return "failed";
        case STATE_ROLLED_BACK: return "rolled_back";
        case STATE_CLEARED: return "cleared";
        default: return "none";
    }
}
