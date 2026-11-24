// E-inx members of HalGPIO that the crosspoint-simulator library lacks.
//
// scripts/simulator_lib_overrides.py includes this file inside the library's
// `class HalGPIO { ... }` (after its WakeupReason enum, to which it appends
// SleepTimer and Button). Device declarations: lib/hal/HalGPIO.h.
// Definitions: src/simulator/HalGPIOSimExtras.cpp.
//
// Not a standalone header: no include guard, no includes.

 public:
  struct DateTime {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 0;
  };

  static constexpr unsigned long BATTERY_POLL_MS = 1500;
  enum class MotionGesture : uint8_t { None, Previous, Next };

  void flushInput();
  void injectOneShotPress(uint8_t buttonIndex) { (void)buttonIndex; }
  bool isAnyPressed() const;
  MotionGesture readMotionGesture(uint8_t orientation, uint8_t mode, uint8_t sensitivity);

  void prepareDeepSleep(uint32_t timerWakeupSeconds = 0, bool retainPowerForButtonGesture = false);
  [[noreturn]] void enterPreparedDeepSleep();
  // No default on the first argument, so startDeepSleep() stays the library's.
  void startDeepSleep(uint32_t timerWakeupSeconds, bool retainPowerForButtonGesture = false);

  struct DeepSleepDiagnostics {
    uint32_t requestedTimerSeconds;
    int32_t gpioSetupResult;
    int32_t timerSetupResult;
    uint32_t wakeStubCount;
    uint32_t timerWakeStubCount;
    uint32_t lastWakeStubCause;
  };

  enum class SleepWakeTraceEvent : uint8_t {
    SleepPlan = 1,
    ArmResult = 2,
    SleepEnter = 3,
    WakeStub = 4,
    EarlyWake = 5,
    DeadlineCheck = 6,
    PowerSample = 7,
    ClassifiedTimer = 8,
    PowerAction = 9,
    SetupWake = 10,
    ImageResult = 11,
    TimerBranch = 12,
    DoublePressBranch = 13,
    ShortPressBranch = 14,
    UiBranch = 15,
    SleepStage = 16,
    PowerGesture = 17,
  };

  struct SleepWakeTraceEntry {
    uint32_t sequence;
    uint32_t rtcTickLow;
    uint32_t arg0;
    uint32_t arg1;
    SleepWakeTraceEvent event;
  };

  static constexpr uint8_t SLEEP_WAKE_TRACE_CAPACITY = 64;

  struct SleepWakeTraceSnapshot {
    uint8_t count = 0;
    SleepWakeTraceEntry entries[SLEEP_WAKE_TRACE_CAPACITY];
  };

  struct SleepTimerTickState {
    uint64_t startTicks = 0;
    uint64_t currentTicks = 0;
    uint64_t durationTicks = 0;
  };

  DeepSleepDiagnostics getDeepSleepDiagnostics() const;
  static uint32_t getLastWakeStubCause();
  static bool lastWakeStubWasTimer();
  static bool sleepTimerDeadlineReached();
  static SleepTimerTickState getSleepTimerTickState();
  static void recordSleepWakeTrace(SleepWakeTraceEvent event, uint32_t arg0 = 0, uint32_t arg1 = 0);
  static SleepWakeTraceSnapshot getSleepWakeTrace();
  static void clearSleepWakeTrace();

  int getBatteryPercentage() const;

  bool readDateTime(DateTime& outDateTime) const;
  bool writeDateTime(const DateTime& dateTime) const;
  bool syncRtcFromSystemTime() const;

  uint64_t getWakeupGpioMask() const;
  bool wakeupIncludedGpio(uint8_t gpioPin) const;
