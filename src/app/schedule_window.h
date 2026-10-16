#pragma once

#include <stdint.h>

inline bool scheduleWindowIsActive(bool enabled,
                                   uint16_t start_min,
                                   uint16_t end_min,
                                   uint16_t current_min) {
    if (!enabled) return false;
    if (start_min == end_min) return false;
    if (current_min > 1439) return false;
    if (start_min > 1439 || end_min > 1439) return false;
    if (start_min < end_min) {
        return current_min >= start_min && current_min < end_min;
    }
    return current_min >= start_min || current_min < end_min;
}
