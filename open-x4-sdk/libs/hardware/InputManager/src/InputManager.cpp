/**
 * @file InputManager.cpp
 * @brief Definitions for InputManager.
 */

#include "InputManager.h"

const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0) {}

void InputManager::begin() {
  pinMode(BUTTON_ADC_PIN_1, INPUT);
  pinMode(BUTTON_ADC_PIN_2, INPUT);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      // Too close to either threshold to be sure which side of it the contact is on. Reporting
      // "no button" here is what turns a mid-transition reading into a dropped sample instead of
      // a press of the neighbouring button.
      const bool nearUpper = ranges[i] - adcValue < ADC_GUARD_BAND;
      // The bottom range is open-ended (INT32_MIN); subtracting from it would overflow, and there
      // is no neighbour below it to be confused with.
      const bool nearLower = ranges[i + 1] != INT32_MIN && adcValue - ranges[i + 1] < ADC_GUARD_BAND;
      if (nearUpper || nearLower) {
        return -1;
      }
      return i;
    }
  }

  return -1;
}

uint8_t InputManager::getState() {
  uint8_t state = 0;

  const int adcValue1 = analogRead(BUTTON_ADC_PIN_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  const int adcValue2 = analogRead(BUTTON_ADC_PIN_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  if (digitalRead(POWER_BUTTON_PIN) == LOW) {
    state |= (1 << BTN_POWER);
  }

  return state;
}

void InputManager::injectOneShotPress(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    pendingInjectPress |= static_cast<uint8_t>(1u << buttonIndex);
  }
}

bool InputManager::readStableState(uint8_t& out) {
  const uint8_t first = getState();

  for (uint8_t i = 1; i < STABLE_SAMPLE_COUNT; i++) {
    delayMicroseconds(STABLE_SAMPLE_GAP_US);
    if (getState() != first) {
      return false;
    }
  }

  out = first;
  return true;
}

void InputManager::update() {
  const unsigned long currentTime = millis();

  pressedEvents = 0;
  releasedEvents = 0;

  uint8_t state = 0;
  if (!readStableState(state)) {
    // The buttons were caught mid-change. Leaving the debounce state untouched means the next
    // call starts from the last settled reading rather than from a transition, so a press is
    // registered once the contact has actually settled and never on the way there.
    if (pendingInjectPress != 0) {
      pressedEvents |= pendingInjectPress;
      pendingInjectPress = 0;
    }
    return;
  }

  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      pressedEvents = state & ~currentState;
      releasedEvents = currentState & ~state;

      if (pressedEvents > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      if (releasedEvents > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      currentState = state;
    }
  }

  if (pendingInjectPress != 0) {
    pressedEvents |= pendingInjectPress;
    pendingInjectPress = 0;
  }
}

void InputManager::flush() {
  const unsigned long currentTime = millis();
  const uint8_t state = getState();
  currentState = state;
  lastState = state;
  pressedEvents = 0;
  releasedEvents = 0;
  pendingInjectPress = 0;
  lastDebounceTime = currentTime;
  buttonPressStart = currentTime;
  buttonPressFinish = currentTime;
}

bool InputManager::isPressed(const uint8_t buttonIndex) const { return currentState & (1 << buttonIndex); }

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }
