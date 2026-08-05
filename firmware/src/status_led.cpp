#include "status_led.h"

#include <Arduino.h>

#include "config.h"

namespace ft26::status_led {
namespace {

constexpr uint32_t LED_TIMER_US = 100000;
constexpr uint16_t SLOW_PERIOD_TICKS = 10;     // 1.0 s
constexpr uint16_t SLOW_ON_TICKS = 2;          // 0.2 s
constexpr uint16_t FAULT_ON_TICKS = 2;         // 0.2 s
constexpr uint16_t FAULT_OFF_TICKS = 2;        // 0.2 s
constexpr uint16_t FAULT_PAUSE_TICKS = 30;     // 3.0 s

hw_timer_t* ledTimer = nullptr;
portMUX_TYPE ledMux = portMUX_INITIALIZER_UNLOCKED;

volatile Mode currentMode = Mode::Off;
volatile FaultGroup currentFault = FaultGroup::None;
volatile bool powerFailLatched = false;
volatile bool ledLevel = false;
volatile uint16_t modeTick = 0;
volatile uint8_t faultPulseIndex = 0;
volatile uint16_t faultTick = 0;
volatile bool faultPause = false;
volatile bool started = false;

void IRAM_ATTR writeLed(bool on) {
  ledLevel = on;
  digitalWrite(ft26::PIN_LED, on ? HIGH : LOW);
}

void IRAM_ATTR resetFaultPattern() {
  faultPulseIndex = 0;
  faultTick = 0;
  faultPause = false;
}

void IRAM_ATTR updateFaultPattern() {
  const uint8_t pulses = static_cast<uint8_t>(currentFault);
  if (pulses == 0) {
    writeLed(false);
    return;
  }

  if (faultPause) {
    writeLed(false);
    ++faultTick;
    if (faultTick >= FAULT_PAUSE_TICKS) {
      resetFaultPattern();
    }
    return;
  }

  const uint16_t phaseTick = faultTick % (FAULT_ON_TICKS + FAULT_OFF_TICKS);
  writeLed(phaseTick < FAULT_ON_TICKS);

  ++faultTick;
  if (faultTick >= (FAULT_ON_TICKS + FAULT_OFF_TICKS)) {
    faultTick = 0;
    ++faultPulseIndex;
    if (faultPulseIndex >= pulses) {
      faultPause = true;
    }
  }
}

void IRAM_ATTR onLedTimer() {
  portENTER_CRITICAL_ISR(&ledMux);

  if (powerFailLatched) {
    writeLed(false);
    portEXIT_CRITICAL_ISR(&ledMux);
    return;
  }

  if (currentFault != FaultGroup::None) {
    updateFaultPattern();
    portEXIT_CRITICAL_ISR(&ledMux);
    return;
  }

  switch (currentMode) {
    case Mode::Off:
      writeLed(false);
      break;
    case Mode::SolidOn:
      writeLed(true);
      break;
    case Mode::SlowPulse:
      writeLed(modeTick < SLOW_ON_TICKS);
      modeTick = (modeTick + 1) % SLOW_PERIOD_TICKS;
      break;
    case Mode::SdWriteToggle:
      break;
  }

  portEXIT_CRITICAL_ISR(&ledMux);
}

void resetModeTimer() {
  modeTick = 0;
}

bool ignoreNormalCommand() {
  return powerFailLatched || currentFault != FaultGroup::None;
}

}  // namespace

void begin() {
  pinMode(ft26::PIN_LED, OUTPUT);
  digitalWrite(ft26::PIN_LED, LOW);

  portENTER_CRITICAL(&ledMux);
  currentMode = Mode::Off;
  currentFault = FaultGroup::None;
  powerFailLatched = false;
  ledLevel = false;
  resetModeTimer();
  resetFaultPattern();
  portEXIT_CRITICAL(&ledMux);

  if (!started) {
    ledTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(ledTimer, &onLedTimer, true);
    timerAlarmWrite(ledTimer, LED_TIMER_US, true);
    timerAlarmEnable(ledTimer);
    started = true;
  }
}

void setMode(Mode mode) {
  portENTER_CRITICAL(&ledMux);
  if (!ignoreNormalCommand()) {
    currentMode = mode;
    resetModeTimer();
    if (mode == Mode::Off) {
      writeLed(false);
    } else if (mode == Mode::SolidOn) {
      writeLed(true);
    }
  }
  portEXIT_CRITICAL(&ledMux);
}

void notifySdWrite() {
  portENTER_CRITICAL(&ledMux);
  if (!ignoreNormalCommand()) {
    currentMode = Mode::SdWriteToggle;
    writeLed(!ledLevel);
  }
  portEXIT_CRITICAL(&ledMux);
}

void setFault(FaultGroup fault) {
  if (fault == FaultGroup::None) {
    return;
  }

  portENTER_CRITICAL(&ledMux);
  if (!powerFailLatched && currentFault == FaultGroup::None) {
    currentFault = fault;
    resetFaultPattern();
  }
  portEXIT_CRITICAL(&ledMux);
}

void powerFailOff() {
  portENTER_CRITICAL(&ledMux);
  powerFailLatched = true;
  writeLed(false);
  portEXIT_CRITICAL(&ledMux);
}

bool isFaultLatched() {
  portENTER_CRITICAL(&ledMux);
  const bool latched = currentFault != FaultGroup::None;
  portEXIT_CRITICAL(&ledMux);
  return latched;
}

FaultGroup latchedFault() {
  portENTER_CRITICAL(&ledMux);
  const FaultGroup fault = currentFault;
  portEXIT_CRITICAL(&ledMux);
  return fault;
}

void clearFaultForManualReset() {
  portENTER_CRITICAL(&ledMux);
  if (!powerFailLatched) {
    currentFault = FaultGroup::None;
    resetFaultPattern();
  }
  portEXIT_CRITICAL(&ledMux);
}

}  // namespace ft26::status_led
