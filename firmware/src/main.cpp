#include <Arduino.h>

#include "boot.h"
#include "recorder.h"
#include "status_led.h"

void setup() {
  ft26::boot::initializeHardware();
  ft26::recorder::begin();
}

void loop() {
  ft26::recorder::tick();
}

