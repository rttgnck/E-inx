// Simulator implementations of the E-inx HalGPIO API that the
// crosspoint-simulator library does not provide. Declared in
// src/simulator/HalGPIOEInxMembers.inc.
#ifdef SIMULATOR

#include <HalGPIO.h>

#include <ctime>

namespace {
HalGPIO::SleepWakeTraceSnapshot simTrace;
uint32_t simTraceSequence = 0;
}  // namespace

void HalGPIO::flushInput() {
  // The library latches edges per frame; one update() drains pending SDL events
  // and beginFrame() clears the latches, the same "forget what was pressed"
  // effect flushInput() has on the device.
  update();
  beginFrame();
}

bool HalGPIO::isAnyPressed() const {
  for (uint8_t i = BTN_BACK; i <= BTN_POWER; i++) {
    if (isPressed(i)) return true;
  }
  return false;
}

HalGPIO::MotionGesture HalGPIO::readMotionGesture(uint8_t, uint8_t, uint8_t) { return MotionGesture::None; }

void HalGPIO::prepareDeepSleep(uint32_t timerWakeupSeconds, bool) {
  recordSleepWakeTrace(SleepWakeTraceEvent::SleepPlan, timerWakeupSeconds, 0);
}

void HalGPIO::enterPreparedDeepSleep() {
  startDeepSleep();
  // The library's startDeepSleep() returns only when the window is closed.
  exit(0);
}

void HalGPIO::startDeepSleep(uint32_t timerWakeupSeconds, bool retainPowerForButtonGesture) {
  prepareDeepSleep(timerWakeupSeconds, retainPowerForButtonGesture);
  startDeepSleep();
}

HalGPIO::DeepSleepDiagnostics HalGPIO::getDeepSleepDiagnostics() const { return DeepSleepDiagnostics{}; }
uint32_t HalGPIO::getLastWakeStubCause() { return 0; }
bool HalGPIO::lastWakeStubWasTimer() { return false; }
bool HalGPIO::sleepTimerDeadlineReached() { return false; }
HalGPIO::SleepTimerTickState HalGPIO::getSleepTimerTickState() { return SleepTimerTickState{}; }

void HalGPIO::recordSleepWakeTrace(SleepWakeTraceEvent event, uint32_t arg0, uint32_t arg1) {
  SleepWakeTraceEntry entry{++simTraceSequence, static_cast<uint32_t>(millis()), arg0, arg1, event};
  if (simTrace.count < SLEEP_WAKE_TRACE_CAPACITY) {
    simTrace.entries[simTrace.count++] = entry;
  } else {
    for (uint8_t i = 1; i < SLEEP_WAKE_TRACE_CAPACITY; i++) simTrace.entries[i - 1] = simTrace.entries[i];
    simTrace.entries[SLEEP_WAKE_TRACE_CAPACITY - 1] = entry;
  }
}

HalGPIO::SleepWakeTraceSnapshot HalGPIO::getSleepWakeTrace() { return simTrace; }
void HalGPIO::clearSleepWakeTrace() { simTrace.count = 0; }

int HalGPIO::getBatteryPercentage() const { return 100; }

bool HalGPIO::readDateTime(DateTime& out) const {
  // The X3 has a DS3231 RTC; the simulator reads the host clock instead.
  const time_t now = time(nullptr);
  struct tm local {};
  if (localtime_r(&now, &local) == nullptr) return false;
  out.year = static_cast<uint16_t>(local.tm_year + 1900);
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
  out.weekday = static_cast<uint8_t>(local.tm_wday);
  return true;
}

// Never sets the host clock.
bool HalGPIO::writeDateTime(const DateTime&) const { return true; }
bool HalGPIO::syncRtcFromSystemTime() const { return true; }

uint64_t HalGPIO::getWakeupGpioMask() const { return 0; }
bool HalGPIO::wakeupIncludedGpio(uint8_t) const { return false; }

#endif  // SIMULATOR
