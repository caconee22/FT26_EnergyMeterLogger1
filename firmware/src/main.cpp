#include <Arduino.h>

#include "boot.h"
#include "status_led.h"

void setup() {
  ft26::boot::initializeHardware();
}

void loop() {
}

