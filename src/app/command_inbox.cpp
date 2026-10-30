#include "command_inbox.h"
#include "../sys/config_store.h"
#include "../algo/fan_speed.h"
#include <string.h>

CommandInbox::CommandInbox() : _count(0), _generation(0), _rejected(0) {
    memset(_slots, 0, sizeof(_slots));
    memset(_queue, 0, sizeof(_queue));
}

bool CommandInbox::valid(const Ticket& ticket) const {
    return ticket.slot < CAPACITY && ticket.generation != 0 &&
           _slots[ticket.slot].generation == ticket.generation &&
           _slots[ticket.slot].state != INVALID;
}

bool CommandInbox::enqueue(const Request& request, uint32_t now_ms,
                           bool receipt_required, Ticket* ticket) {
    Lock lock(_mutex);
    if ((request.kind != MANUAL && request.kind != AUTO) ||
        (request.kind == MANUAL && (request.speed < 1 || request.speed > 32 || request.percent > 100)) ||
        (receipt_required && !ticket)) {
        ++_rejected;
        return false;
    }
    for (uint8_t i = 0; i < CAPACITY; ++i) {
        if (_slots[i].state != INVALID) continue;
        Slot& slot = _slots[i];
        memset(&slot, 0, sizeof(slot));
        if (++_generation == 0) ++_generation;
        slot.generation = _generation;
        slot.state = PENDING;
        slot.queued_at_ms = now_ms;
        slot.receipt_required = receipt_required;
        slot.receipt.request = request;
        _queue[_count++] = i;
        if (ticket) *ticket = {slot.generation, i};
        return true;
    }
    ++_rejected;
    return false;
}

void CommandInbox::removePending(uint8_t slot) {
    for (uint8_t i = 0; i < _count; ++i) {
        if (_queue[i] != slot) continue;
        for (uint8_t j = i + 1; j < _count; ++j) _queue[j - 1] = _queue[j];
        --_count;
        return;
    }
}

bool CommandInbox::claim(uint32_t now_ms, Request* request, Ticket* ticket) {
    if (!request || !ticket) return false;
    Lock lock(_mutex);
    while (_count) {
        const uint8_t index = _queue[0];
        removePending(index);
        Slot& slot = _slots[index];
        if (slot.receipt_required &&
            (uint32_t)(now_ms - slot.queued_at_ms) >= HTTP_ADMISSION_TIMEOUT_MS) {
            slot.state = CANCELLED;
            continue;
        }
        slot.state = CLAIMED;
        *request = slot.receipt.request;
        *ticket = {slot.generation, index};
        return true;
    }
    return false;
}

bool CommandInbox::complete(const Ticket& ticket, const CommandSupervisor::CommandRecord& command) {
    Lock lock(_mutex);
    if (!valid(ticket) || _slots[ticket.slot].state != CLAIMED) return false;
    Slot& slot = _slots[ticket.slot];
    slot.receipt.command = command;
    slot.state = slot.receipt_required ? ACCEPTED : INVALID;
    return true;
}

CommandInbox::State CommandInbox::poll(const Ticket& ticket, Receipt* receipt) const {
    Lock lock(_mutex);
    if (!valid(ticket)) return INVALID;
    const Slot& slot = _slots[ticket.slot];
    if (receipt && slot.state == ACCEPTED) *receipt = slot.receipt;
    return slot.state;
}

bool CommandInbox::cancel(const Ticket& ticket) {
    Lock lock(_mutex);
    if (!valid(ticket) || _slots[ticket.slot].state != PENDING) return false;
    removePending(ticket.slot);
    _slots[ticket.slot].state = CANCELLED;
    return true;
}

void CommandInbox::abandon(const Ticket& ticket) {
    Lock lock(_mutex);
    if (!valid(ticket)) return;
    Slot& slot = _slots[ticket.slot];
    if (slot.state == CLAIMED) { slot.receipt_required = false; return; }
    if (slot.state == PENDING) removePending(ticket.slot);
    slot.state = INVALID;
}

uint32_t CommandInbox::rejectedCount() const {
    Lock lock(_mutex);
    return _rejected;
}

CommandSupervisor::CommandRecord acceptControlRequest(
    CommandSupervisor& supervisor, const CommandInbox::Request& request, uint32_t now_ms) {
    const bool manual = request.kind == CommandInbox::MANUAL;
    const CommandSupervisor::CommandRecord command = manual
        ? supervisor.submitManualFan(request.source, 1, request.speed, now_ms)
        : supervisor.submitAutoClear(request.source, CommandSupervisor::OWNER_AUTO, now_ms, "external auto");
    PersistedControlState state{};
    state.mode = manual ? CONTROL_STATE_MANUAL : CONTROL_STATE_AUTO;
    state.manual_percent = manual ? request.percent : 0;
    state.manual_speed = fanSpeedFromPercent(state.manual_percent, 1, 16);
    ConfigStore::saveControlState(&state);
    return command;
}

bool persistAutoAfterManualEnds(bool manual_active, bool* was_manual) {
    if (!was_manual) return false;
    const bool ended = *was_manual && !manual_active;
    *was_manual = manual_active;
    if (!ended) return false;
    PersistedControlState state{};
    state.mode = CONTROL_STATE_AUTO;
    state.manual_percent = 0;
    state.manual_speed = fanSpeedFromPercent(0, 1, 16);
    ConfigStore::saveControlState(&state);
    return true;
}
