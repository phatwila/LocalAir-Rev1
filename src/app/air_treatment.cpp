#include "air_treatment.h"
#include "application.h"

namespace {

static constexpr pin_t UVC_PIN        = P1S1;
static constexpr pin_t STERIONIZER_PIN = D3;

}  // namespace

void applyAirTreatmentOutputs() {
    pinMode(UVC_PIN, OUTPUT);
    digitalWrite(UVC_PIN, HIGH);
    pinMode(STERIONIZER_PIN, OUTPUT);
    digitalWrite(STERIONIZER_PIN, HIGH);
}
