#pragma once

/**
 * @file InputManager.h
 * @brief Public interface and types for InputManager.
 */

#include <Arduino.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Updates the button states. Should be called regularly in the main loop.
   */
  void update();

  /**
   * Re-baselines the current physical button state and clears pending edge events.
   */
  void flush();

  /** Queue a one-shot press for the next update() (e.g. BLE HID → same path as physical buttons). */
  void injectOneShotPress(uint8_t buttonIndex);

  /**
   * Returns true if the button was being held at the time of the last #update() call.
   *
   * @param buttonIndex the button indexes
   * @return the button current press state
   */
  bool isPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if the button went from unpressed to pressed between the last two #update() calls.
   *
   * This differs from #isPressed() in that pressing and holding a button will cause this function
   * to return true after the first #update() call, but false on subsequent calls, whereas #isPressed()
   * will continue to return true.
   *
   * @param buttonIndex
   * @return the button pressed state
   */
  bool wasPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if any button started being pressed between the last two #update() calls
   *
   * @return true if any button started being pressed between the last two #update() calls
   */
  bool wasAnyPressed() const;

  /**
   * Returns true if the button went from pressed to unpressed between the last two #update() calls
   *
   * @param buttonIndex the button indexes
   * @return the button release state
   */
  bool wasReleased(uint8_t buttonIndex) const;

  /**
   * Returns true if any button was released between the last two #update() calls
   *
   * @return  true if any button was released between the last two #update() calls
   */
  bool wasAnyReleased() const;

  /**
   * Returns the time between any button starting to be depressed and all buttons between released
   *
   * @return duration in milliseconds
   */
  unsigned long getHeldTime() const;

  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = 3;

  bool isPowerButtonPressed() const;

  static const char* getButtonName(uint8_t buttonIndex);

 private:
  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);

  /**
   * Samples the buttons several times in a row and only reports a reading the samples agree on.
   *
   * The alternative — trusting one sample once DEBOUNCE_DELAY of wall clock has elapsed — counts
   * time rather than agreement, and the callers do not sample at a steady rate: a screen painting
   * a full e-ink refresh goes several hundred milliseconds without calling update() at all, which
   * satisfies the elapsed-time test on the very first sample afterwards. That sample can easily
   * land mid-press, and because the ADC ladder's ranges are contiguous it then decodes as a
   * neighbouring button rather than as nothing.
   *
   * Sampling happens inside one call, deliberately, so the guarantee does not depend on how often
   * the caller gets round to updating.
   *
   * @return false when the samples disagreed, in which case `out` is untouched.
   */
  bool readStableState(uint8_t& out);

  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  uint8_t pendingInjectPress{0};
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3800;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;

  /** Samples per update(), and the gap between them. Three costs about 1.2 ms. */
  static constexpr uint8_t STABLE_SAMPLE_COUNT = 3;
  static constexpr unsigned int STABLE_SAMPLE_GAP_US = 600;

  /**
   * Dead zone either side of an ADC threshold, in counts.
   *
   * The ranges are contiguous, so without this every reading decodes as *some* button and a
   * contact that is still making or breaking reads as whichever neighbour it happens to pass
   * through. Sixty counts is about 8% of the narrowest gap between thresholds (3800/3100/2090/750).
   */
  static constexpr int ADC_GUARD_BAND = 60;

  static const char* BUTTON_NAMES[];
};
