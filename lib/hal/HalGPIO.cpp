/**
 * @file HalGPIO.cpp
 * @brief Definitions for HalGPIO.
 */

#include <HalGPIO.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_private/esp_clk.h>
#include <esp_sleep.h>
#include <esp_wake_stub.h>
#include <soc/rtc.h>
#include <time.h>

#include <algorithm>
#include <cstdlib>
#include <type_traits>

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readI2CRegs(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, len, static_cast<uint8_t>(true)) < len) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) {
    out[i] = Wire.read();
  }
  return true;
}

bool writeI2CRegs(uint8_t addr, uint8_t reg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission(true) == 0;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

bool readBQ27220StateOfCharge(uint16_t* outSoc) { return readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, outSoc); }

void beginX3I2C() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
}

void endX3I2C() {
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
}

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    return false;
  }
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;
  return tensDigit <= 5 && onesDigit <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  beginX3I2C();

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  endX3I2C();
  return result;
}

}  // namespace X3GPIO

namespace {

RTC_DATA_ATTR volatile uint32_t rtcDeepSleepRequestedTimerSeconds = 0;
RTC_DATA_ATTR volatile int32_t rtcDeepSleepGpioSetupResult = ESP_OK;
RTC_DATA_ATTR volatile int32_t rtcDeepSleepTimerSetupResult = ESP_OK;
RTC_DATA_ATTR volatile uint32_t rtcDeepSleepWakeStubCount = 0;
RTC_DATA_ATTR volatile uint32_t rtcDeepSleepTimerWakeStubCount = 0;
RTC_DATA_ATTR volatile uint32_t rtcDeepSleepLastWakeStubCause = ESP_SLEEP_WAKEUP_UNDEFINED;
RTC_DATA_ATTR volatile uint64_t rtcDeepSleepTimerStartTicks = 0;
RTC_DATA_ATTR volatile uint64_t rtcDeepSleepTimerDurationTicks = 0;

constexpr uint32_t SLEEP_WAKE_TRACE_MAGIC = 0x53574C32UL;

struct SleepWakeTraceState {
  uint32_t magic;
  uint32_t nextSequence;
  uint8_t writeIndex;
  uint8_t count;
  uint16_t reserved;
  HalGPIO::SleepWakeTraceEntry entries[HalGPIO::SLEEP_WAKE_TRACE_CAPACITY];
};

static_assert(std::is_trivial<HalGPIO::SleepWakeTraceEntry>::value,
              "RTC trace entries must not run constructors after a watchdog reset");
static_assert(std::is_trivial<SleepWakeTraceState>::value,
              "RTC trace state must remain untouched by startup initialization");

// Unlike RTC_DATA_ATTR, RTC_NOINIT_ATTR also survives watchdog/software resets. This lets the flight recorder
// retain the events immediately preceding a crash while entering or rotating the sleep screen.
RTC_NOINIT_ATTR volatile SleepWakeTraceState rtcSleepWakeTrace;

constexpr char HW_NAMESPACE[] = "inxhw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    return nvsToDeviceType(cachedValue);
  }

  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  delay(2);
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();
  const bool x3Confirmed = pass1.score() >= 2 && pass2.score() >= 2;
  const bool x4Confirmed = pass1.score() == 0 && pass2.score() == 0;

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }
  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }
  return HalGPIO::DeviceType::X4;
}

uint8_t bcdToDec(uint8_t value) { return ((value >> 4) * 10) + (value & 0x0F); }

uint8_t decToBcd(uint8_t value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }

bool validDateTime(const HalGPIO::DateTime& dt) {
  return dt.year >= 2024 && dt.year <= 2099 && dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31 &&
         dt.hour <= 23 && dt.minute <= 59 && dt.second <= 59;
}

void RTC_IRAM_ATTR appendSleepWakeTraceAtTick(const HalGPIO::SleepWakeTraceEvent event, const uint32_t arg0,
                                              const uint32_t arg1, const uint32_t rtcTickLow) {
  if (rtcSleepWakeTrace.magic != SLEEP_WAKE_TRACE_MAGIC) {
    rtcSleepWakeTrace.magic = SLEEP_WAKE_TRACE_MAGIC;
    rtcSleepWakeTrace.nextSequence = 1;
    rtcSleepWakeTrace.writeIndex = 0;
    rtcSleepWakeTrace.count = 0;
  }

  const uint8_t index = rtcSleepWakeTrace.writeIndex;
  volatile HalGPIO::SleepWakeTraceEntry& entry = rtcSleepWakeTrace.entries[index];
  entry.sequence = rtcSleepWakeTrace.nextSequence++;
  entry.rtcTickLow = rtcTickLow;
  entry.arg0 = arg0;
  entry.arg1 = arg1;
  entry.event = event;

  uint8_t nextIndex = static_cast<uint8_t>(index + 1);
  if (nextIndex >= HalGPIO::SLEEP_WAKE_TRACE_CAPACITY) {
    nextIndex = 0;
  }
  rtcSleepWakeTrace.writeIndex = nextIndex;
  if (rtcSleepWakeTrace.count < HalGPIO::SLEEP_WAKE_TRACE_CAPACITY) {
    rtcSleepWakeTrace.count++;
  }
}

void appendSleepWakeTrace(const HalGPIO::SleepWakeTraceEvent event, const uint32_t arg0, const uint32_t arg1) {
  appendSleepWakeTraceAtTick(event, arg0, arg1, static_cast<uint32_t>(rtc_time_get()));
}

}  // namespace

extern "C" void RTC_IRAM_ATTR esp_wake_deep_sleep() {
  esp_default_wake_deep_sleep();
  const uint32_t wakeCause = esp_wake_stub_get_wakeup_cause();
  rtcDeepSleepLastWakeStubCause = wakeCause;
  rtcDeepSleepWakeStubCount++;
  if ((wakeCause & RTC_TIMER_TRIG_EN) != 0) {
    rtcDeepSleepTimerWakeStubCount++;
  }
  // rtc_time_get() can block before normal clock initialization on the X3. A zero timestamp keeps this wake-stub
  // record self-contained in RTC memory; the following early-boot event provides the post-wake timestamp.
  appendSleepWakeTraceAtTick(HalGPIO::SleepWakeTraceEvent::WakeStub, wakeCause, rtcDeepSleepWakeStubCount, 0);
}

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  deviceType = detectDeviceTypeWithFingerprint();
  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
}

void HalGPIO::update() { inputMgr.update(); }

void HalGPIO::flushInput() { inputMgr.flush(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::isAnyPressed() const {
  for (uint8_t i = 0; i <= BTN_POWER; ++i) {
    if (inputMgr.isPressed(i)) {
      return true;
    }
  }
  return false;
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

HalGPIO::MotionGesture HalGPIO::readMotionGesture(const uint8_t orientation, const uint8_t mode,
                                                  const uint8_t sensitivity) {
  if (!deviceIsX3()) {
    return MotionGesture::None;
  }
  if (mode == 0 && !motionSensorInitialized) {
    return MotionGesture::None;
  }

  const unsigned long now = millis();
  if (motionLastPollMs != 0 && now - motionLastPollMs < 50) {
    return MotionGesture::None;
  }
  motionLastPollMs = now;

  X3GPIO::beginX3I2C();
  if (!motionSensorInitialized) {
    uint8_t whoami = 0;
    if (X3GPIO::readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
      motionSensorAddress = I2C_ADDR_QMI8658;
    } else if (X3GPIO::readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) &&
               whoami == QMI8658_WHO_AM_I_VALUE) {
      motionSensorAddress = I2C_ADDR_QMI8658_ALT;
    } else {
      X3GPIO::endX3I2C();
      return MotionGesture::None;
    }

    const uint8_t ctrl1 = 0x60;  // Register auto-increment.
    const uint8_t ctrl3 = 0x58;  // Gyroscope: +/-512 dps, 28 Hz.
    const uint8_t ctrl7 = 0x02;  // Enable gyroscope only.
    const bool configured = X3GPIO::writeI2CRegs(motionSensorAddress, QMI8658_CTRL1_REG, &ctrl1, 1) &&
                            X3GPIO::writeI2CRegs(motionSensorAddress, QMI8658_CTRL3_REG, &ctrl3, 1) &&
                            X3GPIO::writeI2CRegs(motionSensorAddress, QMI8658_CTRL7_REG, &ctrl7, 1);
    if (!configured) {
      motionSensorAddress = 0;
      X3GPIO::endX3I2C();
      return MotionGesture::None;
    }
    motionSensorInitialized = true;
    motionSensorStartedMs = now;
    motionLastGestureMs = now;
  }

  if (mode == 0) {
    const uint8_t ctrl7 = 0x00;
    const uint8_t ctrl1 = 0x61;
    X3GPIO::writeI2CRegs(motionSensorAddress, QMI8658_CTRL7_REG, &ctrl7, 1);
    X3GPIO::writeI2CRegs(motionSensorAddress, QMI8658_CTRL1_REG, &ctrl1, 1);
    motionSensorInitialized = false;
    motionGestureInProgress = false;
    X3GPIO::endX3I2C();
    return MotionGesture::None;
  }

  uint8_t raw[6] = {};
  const bool readOk =
      X3GPIO::readI2CRegs(motionSensorAddress, QMI8658_GYRO_X_L_REG, raw, static_cast<uint8_t>(sizeof(raw)));
  X3GPIO::endX3I2C();
  if (!readOk) {
    return MotionGesture::None;
  }

  if (now - motionSensorStartedMs < 300) {
    return MotionGesture::None;
  }

  const int16_t gx = static_cast<int16_t>((static_cast<uint16_t>(raw[1]) << 8) | raw[0]);
  const int16_t gy = static_cast<int16_t>((static_cast<uint16_t>(raw[3]) << 8) | raw[2]);
  int32_t axis = 0;
  switch (orientation) {
    case 1:  // Landscape clockwise
      axis = -static_cast<int32_t>(gy);
      break;
    case 2:  // Portrait inverted
      axis = -static_cast<int32_t>(gx);
      break;
    case 3:  // Landscape counter-clockwise
      axis = gy;
      break;
    default:
      axis = gx;
      break;
  }
  if (mode == 2) {
    axis = -axis;
  }

  constexpr int32_t kGyroLsbPerDps = 64;
  constexpr int32_t kNeutralThreshold = 50 * kGyroLsbPerDps;
  const int32_t triggerDps = sensitivity >= 2 ? 180 : sensitivity == 0 ? 360 : 270;
  const int32_t triggerThreshold = triggerDps * kGyroLsbPerDps;

  if (motionGestureInProgress) {
    if (std::abs(axis) < kNeutralThreshold) {
      motionGestureInProgress = false;
    }
    return MotionGesture::None;
  }

  if (now - motionLastGestureMs < 600) {
    return MotionGesture::None;
  }

  if (axis > triggerThreshold) {
    motionGestureInProgress = true;
    motionLastGestureMs = now;
    return MotionGesture::Next;
  }
  if (axis < -triggerThreshold) {
    motionGestureInProgress = true;
    motionLastGestureMs = now;
    return MotionGesture::Previous;
  }
  return MotionGesture::None;
}

void HalGPIO::prepareDeepSleep(const uint32_t timerWakeupSeconds, const bool retainPowerForButtonGesture) {
  // Keep the entry sequence identical to the physically proven v1.0.20-7 path.
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }

  // The X3's GPIO13 drives its battery latch. Letting the pin float in deep sleep powers the MCU completely
  // off, including its RTC timer. Retain the latch only when an automatic wake is actually requested; timer-off
  // sleeps keep the stock full-power-off behavior and its lower battery drain.
  const bool retainX3BatteryPower =
      deviceIsX3() && (timerWakeupSeconds > 0 || retainPowerForButtonGesture);
  if (deviceIsX3()) {
    constexpr gpio_num_t X3_BATTERY_LATCH_GPIO = GPIO_NUM_13;
    gpio_set_direction(X3_BATTERY_LATCH_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(X3_BATTERY_LATCH_GPIO, 1);
    gpio_hold_dis(X3_BATTERY_LATCH_GPIO);
    gpio_deep_sleep_hold_dis();
    gpio_set_direction(X3_BATTERY_LATCH_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(X3_BATTERY_LATCH_GPIO, 1);
    if (retainX3BatteryPower) {
      gpio_hold_en(X3_BATTERY_LATCH_GPIO);
      gpio_deep_sleep_hold_en();
    }
  }
  appendSleepWakeTrace(SleepWakeTraceEvent::SleepStage, 36,
                       (deviceIsX3() ? 1UL : 0UL) | (retainX3BatteryPower ? 2UL : 0UL));

  rtcDeepSleepRequestedTimerSeconds = timerWakeupSeconds;
  rtcDeepSleepLastWakeStubCause = ESP_SLEEP_WAKEUP_UNDEFINED;
  const uint64_t wakeMask = 1ULL << InputManager::POWER_BUTTON_PIN;
  const esp_err_t gpioWakeResult = esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_LOW);
  rtcDeepSleepGpioSetupResult = gpioWakeResult;
  if (gpioWakeResult != ESP_OK) {
    if (Serial) {
      Serial.printf("[%lu] [HAL] Power wake setup failed err=%d\n", millis(), static_cast<int>(gpioWakeResult));
    }
  }

  if (timerWakeupSeconds > 0) {
    const uint64_t timerDurationUs = static_cast<uint64_t>(timerWakeupSeconds) * 1000000ULL;
    const esp_err_t timerWakeResult = esp_sleep_enable_timer_wakeup(timerDurationUs);
    rtcDeepSleepTimerSetupResult = timerWakeResult;
    if (timerWakeResult == ESP_OK) {
      // Classification witness only. Raw RTC ticks survive deep sleep and need no early-boot calibration.
      const uint32_t slowClockPeriod = esp_clk_slowclk_cal_get();
      rtcDeepSleepTimerStartTicks = rtc_time_get();
      rtcDeepSleepTimerDurationTicks =
          slowClockPeriod == 0 ? 0 : rtc_time_us_to_slowclk(timerDurationUs, slowClockPeriod);
    } else {
      rtcDeepSleepTimerStartTicks = 0;
      rtcDeepSleepTimerDurationTicks = 0;
      if (Serial) {
        Serial.printf("[%lu] [HAL] Timer wake setup failed seconds=%lu err=%d\n", millis(),
                      static_cast<unsigned long>(timerWakeupSeconds), static_cast<int>(timerWakeResult));
      }
    }
  } else {
    rtcDeepSleepTimerSetupResult = ESP_OK;
    rtcDeepSleepTimerStartTicks = 0;
    rtcDeepSleepTimerDurationTicks = 0;
  }

  appendSleepWakeTrace(SleepWakeTraceEvent::ArmResult, timerWakeupSeconds,
                       (static_cast<uint32_t>(gpioWakeResult) << 16) |
                           (static_cast<uint32_t>(rtcDeepSleepTimerSetupResult) & 0xFFFFUL));
  appendSleepWakeTrace(SleepWakeTraceEvent::SleepEnter, static_cast<uint32_t>(rtcDeepSleepTimerStartTicks),
                       static_cast<uint32_t>(rtcDeepSleepTimerDurationTicks));
}

void HalGPIO::enterPreparedDeepSleep() {
  esp_deep_sleep_start();
  __builtin_unreachable();
}

void HalGPIO::startDeepSleep(const uint32_t timerWakeupSeconds, const bool retainPowerForButtonGesture) {
  prepareDeepSleep(timerWakeupSeconds, retainPowerForButtonGesture);
  enterPreparedDeepSleep();
}

HalGPIO::DeepSleepDiagnostics HalGPIO::getDeepSleepDiagnostics() const {
  return {rtcDeepSleepRequestedTimerSeconds, rtcDeepSleepGpioSetupResult, rtcDeepSleepTimerSetupResult,
          rtcDeepSleepWakeStubCount, rtcDeepSleepTimerWakeStubCount, rtcDeepSleepLastWakeStubCause};
}

uint32_t HalGPIO::getLastWakeStubCause() { return rtcDeepSleepLastWakeStubCause; }

bool HalGPIO::lastWakeStubWasTimer() { return (rtcDeepSleepLastWakeStubCause & RTC_TIMER_TRIG_EN) != 0; }

bool HalGPIO::sleepTimerDeadlineReached() {
  const SleepTimerTickState state = getSleepTimerTickState();
  return state.durationTicks != 0 && state.currentTicks - state.startTicks >= state.durationTicks;
}

HalGPIO::SleepTimerTickState HalGPIO::getSleepTimerTickState() {
  SleepTimerTickState state;
  state.startTicks = rtcDeepSleepTimerStartTicks;
  state.currentTicks = rtc_time_get();
  state.durationTicks = rtcDeepSleepTimerDurationTicks;
  return state;
}

void HalGPIO::recordSleepWakeTrace(const SleepWakeTraceEvent event, const uint32_t arg0, const uint32_t arg1) {
  appendSleepWakeTrace(event, arg0, arg1);
}

HalGPIO::SleepWakeTraceSnapshot HalGPIO::getSleepWakeTrace() {
  SleepWakeTraceSnapshot snapshot;
  if (rtcSleepWakeTrace.magic != SLEEP_WAKE_TRACE_MAGIC) {
    return snapshot;
  }

  snapshot.count = rtcSleepWakeTrace.count;
  uint8_t index =
      snapshot.count == SLEEP_WAKE_TRACE_CAPACITY ? rtcSleepWakeTrace.writeIndex : static_cast<uint8_t>(0);
  for (uint8_t i = 0; i < snapshot.count; ++i) {
    const volatile SleepWakeTraceEntry& source = rtcSleepWakeTrace.entries[index];
    SleepWakeTraceEntry& destination = snapshot.entries[i];
    destination.sequence = source.sequence;
    destination.rtcTickLow = source.rtcTickLow;
    destination.arg0 = source.arg0;
    destination.arg1 = source.arg1;
    destination.event = source.event;
    index++;
    if (index >= SLEEP_WAKE_TRACE_CAPACITY) {
      index = 0;
    }
  }
  return snapshot;
}

void HalGPIO::clearSleepWakeTrace() {
  rtcSleepWakeTrace.magic = SLEEP_WAKE_TRACE_MAGIC;
  rtcSleepWakeTrace.nextSequence = 1;
  rtcSleepWakeTrace.writeIndex = 0;
  rtcSleepWakeTrace.count = 0;
}

int HalGPIO::getBatteryPercentage() const {
  if (deviceIsX3()) {
    const unsigned long now = millis();
    if (batteryLastPollMs != 0 && (now - batteryLastPollMs) < BATTERY_POLL_MS) {
      return batteryCachedPercent;
    }

    uint16_t soc = 0;
    X3GPIO::beginX3I2C();
    const bool ok = X3GPIO::readBQ27220StateOfCharge(&soc);
    X3GPIO::endX3I2C();
    if (ok && soc <= 100) {
      batteryCachedPercent = static_cast<int>(soc);
      batteryLastPollMs = now;
      return batteryCachedPercent;
    }
    batteryLastPollMs = now;
    return batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  return battery.readPercentage();
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    X3GPIO::beginX3I2C();
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        X3GPIO::endX3I2C();
        return currentMa > 0;
      }
      delay(2);
    }
    X3GPIO::endX3I2C();
    return false;
  }
  return digitalRead(UART0_RXD) == HIGH;
}

bool HalGPIO::readDateTime(DateTime& outDateTime) const {
  if (!deviceIsX3()) {
    return false;
  }

  uint8_t regs[7] = {};
  X3GPIO::beginX3I2C();
  const bool ok = X3GPIO::readI2CRegs(I2C_ADDR_DS3231, DS3231_SEC_REG, regs, sizeof(regs));
  X3GPIO::endX3I2C();
  if (!ok) {
    return false;
  }

  DateTime dt;
  dt.second = bcdToDec(regs[0] & 0x7F);
  dt.minute = bcdToDec(regs[1] & 0x7F);
  dt.hour = bcdToDec(regs[2] & 0x3F);
  dt.weekday = bcdToDec(regs[3] & 0x07);
  dt.day = bcdToDec(regs[4] & 0x3F);
  dt.month = bcdToDec(regs[5] & 0x1F);
  dt.year = static_cast<uint16_t>(2000 + bcdToDec(regs[6]));

  if (!validDateTime(dt)) {
    return false;
  }

  outDateTime = dt;
  return true;
}

bool HalGPIO::writeDateTime(const DateTime& dateTime) const {
  if (!deviceIsX3() || !validDateTime(dateTime)) {
    return false;
  }

  const uint8_t regs[7] = {decToBcd(dateTime.second),
                           decToBcd(dateTime.minute),
                           decToBcd(dateTime.hour),
                           decToBcd(dateTime.weekday >= 1 && dateTime.weekday <= 7 ? dateTime.weekday : 1),
                           decToBcd(dateTime.day),
                           decToBcd(dateTime.month),
                           decToBcd(static_cast<uint8_t>(dateTime.year - 2000))};
  X3GPIO::beginX3I2C();
  const bool ok = X3GPIO::writeI2CRegs(I2C_ADDR_DS3231, DS3231_SEC_REG, regs, sizeof(regs));
  X3GPIO::endX3I2C();
  return ok;
}

bool HalGPIO::syncRtcFromSystemTime() const {
  if (!deviceIsX3()) {
    return false;
  }

  const time_t now = time(nullptr);
  if (now < 1704067200) {
    return false;
  }

  struct tm localTime{};
  if (localtime_r(&now, &localTime) == nullptr) {
    return false;
  }

  DateTime dt;
  dt.year = static_cast<uint16_t>(localTime.tm_year + 1900);
  dt.month = static_cast<uint8_t>(localTime.tm_mon + 1);
  dt.day = static_cast<uint8_t>(localTime.tm_mday);
  dt.hour = static_cast<uint8_t>(localTime.tm_hour);
  dt.minute = static_cast<uint8_t>(localTime.tm_min);
  dt.second = static_cast<uint8_t>(localTime.tm_sec);
  dt.weekday = static_cast<uint8_t>(localTime.tm_wday == 0 ? 7 : localTime.tm_wday);
  return writeDateTime(dt);
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // The wakeup cause is the authoritative source. Some X3 boots report a reset reason that does not survive
  // early startup consistently; requiring both fields can demote a real timer wake to Other/Power.
  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
    return WakeupReason::SleepTimer;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) {
    if (wakeupIncludedGpio(InputManager::POWER_BUTTON_PIN) ||
        digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      return WakeupReason::PowerButton;
    }
    return WakeupReason::Button;
  }
  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}

uint64_t HalGPIO::getWakeupGpioMask() const { return esp_sleep_get_gpio_wakeup_status(); }

bool HalGPIO::wakeupIncludedGpio(const uint8_t gpioPin) const {
  return (getWakeupGpioMask() & (1ULL << gpioPin)) != 0;
}
