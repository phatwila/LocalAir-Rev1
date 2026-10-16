#include "sense_burst.h"

SenseBurst::SenseBurst()
    : _active(false), _complete(false), _completed_by_sensor(false),
      _timed_out(false), _start_ms(0) {}

void SenseBurst::start(uint32_t now_ms) {
    _active = true;
    _complete = false;
    _completed_by_sensor = false;
    _timed_out = false;
    _start_ms = now_ms;
}

void SenseBurst::completeBySensor(uint32_t) {
    _active = false; _complete = true;
    _completed_by_sensor = true; _timed_out = false;
}

void SenseBurst::completeByTimeout(uint32_t) {
    _active = false; _complete = true;
    _completed_by_sensor = false; _timed_out = true;
}

void SenseBurst::cancel() {
    _active = false; _complete = true;
    _completed_by_sensor = false; _timed_out = false;
}

uint32_t SenseBurst::elapsedMs(uint32_t now_ms) const {
    return now_ms - _start_ms;
}
