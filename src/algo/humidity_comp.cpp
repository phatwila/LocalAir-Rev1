#include "humidity_comp.h"
#include <math.h>

// Buck equation: saturation vapour pressure (kPa)
static float e_sat(float T) {
    return 0.61078f * expf((17.27f * T) / (T + 237.3f));
}

float HumidityComp::absoluteHumidity(float temp_c, float rh_pct) {
    // AH in g/m³
    float es = e_sat(temp_c);        // kPa
    float ea = es * (rh_pct / 100.0f);
    return (ea * 1000.0f * 18.016f) / (8.314f * (temp_c + 273.15f));
}

float HumidityComp::factor(float temp_c, float rh_pct) {
    // SGP30 reads high under elevated humidity.
    // Empirically: at 25°C/50%RH the over-read is ~18%.
    // Factor = 1 + k*(AH - AH_ref), where AH_ref = dry-ish reference ~7 g/m³.
    float ah = absoluteHumidity(temp_c, rh_pct);
    float ah_ref = 7.0f;
    float k = 0.025f;  // tuned from SGP30 datasheet correction curves
    float f = 1.0f + k * (ah - ah_ref);
    if (f < 0.5f) f = 0.5f;
    if (f > 2.5f) f = 2.5f;
    return f;
}

uint16_t HumidityComp::sgp30HumidityWord(float temp_c, float rh_pct) {
    // SGP30 expects humidity as 8.8 bit fixed-point (g/m³ × 256).
    float ah = absoluteHumidity(temp_c, rh_pct);
    if (ah < 0.0f) ah = 0.0f;
    if (ah > 255.9f) ah = 255.9f;
    return (uint16_t)(ah * 256.0f);
}
