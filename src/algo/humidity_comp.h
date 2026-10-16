#pragma once
#include <stdint.h>

class HumidityComp {
public:
    static float factor(float temp_c, float rh_pct);
    static float absoluteHumidity(float temp_c, float rh_pct);
    static uint16_t sgp30HumidityWord(float temp_c, float rh_pct);
};
