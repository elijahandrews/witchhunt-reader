#pragma once

#include "CrossPointSettings.h"

// UI option indices may skip capabilities; persisted BUTTON_ACTION IDs never do.
// This is also used when pruning mappings from an SD card moved to another board.
struct ButtonActionCapabilities {
  bool light;
  bool warmLight;
  bool usbDrive;
};

constexpr bool buttonActionAvailable(const uint8_t action, const ButtonActionCapabilities caps) {
  using S = CrossPointSettings;
  switch (action) {
    case S::BTN_LIGHT_TOGGLE:
    case S::BTN_LIGHT_BRIGHTER:
    case S::BTN_LIGHT_DIMMER:
      return caps.light;
    case S::BTN_LIGHT_WARMER:
    case S::BTN_LIGHT_COOLER:
      return caps.light && caps.warmLight;
    case S::BTN_USB_DRIVE:
      return caps.usbDrive;
    default:
      return action < S::BUTTON_ACTION_COUNT;
  }
}

constexpr uint8_t buttonActionAtOption(const uint8_t option, const ButtonActionCapabilities caps) {
  uint8_t index = 0;
  for (uint8_t action = 0; action < CrossPointSettings::BUTTON_ACTION_COUNT; ++action) {
    if (buttonActionAvailable(action, caps) && index++ == option) return action;
  }
  return CrossPointSettings::BTN_DEFAULT;
}

constexpr uint8_t buttonActionOption(const uint8_t selected, const ButtonActionCapabilities caps) {
  uint8_t index = 0;
  for (uint8_t action = 0; action < CrossPointSettings::BUTTON_ACTION_COUNT; ++action) {
    if (!buttonActionAvailable(action, caps)) continue;
    if (action == selected) return index;
    ++index;
  }
  return 0;
}
