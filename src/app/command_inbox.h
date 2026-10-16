#pragma once

#include "application.h"
#include "command_supervisor.h"

// Cross-thread ingress only. The application loop owns command execution.
class CommandInbox {
public:
    static constexpr uint8_t CAPACITY = 8;
    static constexpr uint32_t HTTP_ADMISSION_TIMEOUT_MS = 3000;
    enum Kind : uint8_t { MANUAL, AUTO };
    struct Request {
        Kind kind;
        uint8_t source;
        uint8_t speed;
        uint8_t percent;
    };
    struct Ticket { uint32_t generation; uint8_t slot; };
    struct Receipt { Request request; CommandSupervisor::CommandRecord command; };
    enum State : uint8_t { INVALID, PENDING, CLAIMED, ACCEPTED, CANCELLED };

    CommandInbox();
    bool enqueue(const Request& request, uint32_t now_ms, bool receipt_required,
                 Ticket* ticket = nullptr);
    bool claim(uint32_t now_ms, Request* request, Ticket* ticket);
    bool complete(const Ticket& ticket, const CommandSupervisor::CommandRecord& command);
    State poll(const Ticket& ticket, Receipt* receipt = nullptr) const;
    bool cancel(const Ticket& ticket);
    // A disconnected waiter relinquishes storage. Claimed requests still finish.
    void abandon(const Ticket& ticket);
    uint32_t rejectedCount() const;

private:
    struct Slot {
        State state;
        uint32_t generation;
        uint32_t queued_at_ms;
        bool receipt_required;
        Receipt receipt;
    };
    struct Lock {
        RecursiveMutex& mutex;
        explicit Lock(RecursiveMutex& m) : mutex(m) { mutex.lock(); }
        ~Lock() { mutex.unlock(); }
    };
    mutable RecursiveMutex _mutex;
    Slot _slots[CAPACITY];
    uint8_t _queue[CAPACITY];
    uint8_t _count;
    uint32_t _generation;
    uint32_t _rejected;
    bool valid(const Ticket& ticket) const;
    void removePending(uint8_t slot);
};

// Application-owner acceptance. Receipt publication must follow this call and
// the owner's startup/status/audit updates. No network or hardware I/O occurs.
CommandSupervisor::CommandRecord acceptControlRequest(
    CommandSupervisor& supervisor, const CommandInbox::Request& request, uint32_t now_ms);
