#include <iostream>

using S = CrossPointSettings;
using B = MappedInputManager::Button;
using P = ButtonEventManager::PressType;

static SettingInfo row(const char* key) {
  SettingInfo found;
  bool present = false;
  forEachSetting([&](const SettingInfo& value) {
    if (value.key && std::strcmp(value.key, key) == 0) {
      found = value;
      present = true;
    }
  });
  assert(present);
  return found;
}

static std::vector<ButtonEventManager::ButtonEvent> events(ButtonEventManager& buttons) {
  buttons.update();
  std::vector<ButtonEventManager::ButtonEvent> result;
  ButtonEventManager::ButtonEvent event;
  while (buttons.consumeEvent(event)) result.push_back(event);
  return result;
}

int main() {
  static_assert(S::BTN_LIGHT_TOGGLE == 28 && S::BTN_LIGHT_COOLER == 32);
  static_assert(S::BTN_TOGGLE_SETTINGS == 33 && S::BTN_USB_DRIVE == 34);
  static_assert(S::BTN_TOGGLE_STATUS_BAR == 35);
  // Loading older settings preserves upstream's enum-to-point migration, including
  // native family sizes that are absent from the built-in ladder.
  assert(JsonSettingsIO::loadSettings(SETTINGS, "{\"fontSizeOrderV\":0,\"fontSize\":0,\"txtFontSize\":1}"));
  assert(SETTINGS.fontPointSize == 12 && SETTINGS.txtFontPointSize == 14);
  assert(JsonSettingsIO::loadSettings(SETTINGS, "{\"fontSizeOrderV\":1,\"fontSize\":0,\"txtFontSize\":1}"));
  assert(SETTINGS.fontPointSize == 10 && SETTINGS.txtFontPointSize == 12);
  assert(JsonSettingsIO::loadSettings(SETTINGS, "{\"fontSizeOrderV\":2,\"fontSize\":13,\"txtFontSize\":15}"));
  assert(SETTINGS.fontPointSize == 13 && SETTINGS.txtFontPointSize == 15);
  assert(SETTINGS.statusBarVisible == 1);
  auto visible = row("statusBarVisible");
  assert(visible.valuePtr == &S::statusBarVisible);
  SETTINGS.statusBarVisible = 0;
  assert(JsonSettingsIO::saveSettings(SETTINGS, "visibility"));
  SETTINGS.statusBarVisible = 1;
  assert(JsonSettingsIO::loadSettings(SETTINGS, Storage.files.at("visibility").c_str()));
  assert(SETTINGS.statusBarVisible == 0);
  const uint8_t defaultPower = SETTINGS.btnDoublePower;
  const uint8_t defaultPrevious = SETTINGS.btnDoublePageBack;
  // All capability combinations: no renumbering old light settings and no
  // USB option on hardware that cannot enter mass storage.
  for (bool light : {false, true})
    for (bool warm : {false, true})
      for (bool usb : {false, true}) {
        SETTINGS.btnDoublePower = defaultPower;
        SETTINGS.btnDoublePageBack = defaultPrevious;
        Frontlight.light = light;
        Frontlight.warm = warm;
        hostUsb = usb;
        const ButtonActionCapabilities caps{light, warm, usb};
        auto power = row("btnDoublePower");
        auto previous = row("btnDoublePageBack");
        assert(power.valuePtr == nullptr && power.persistPtr == &S::btnDoublePower);
        unsigned count = 0;
        for (unsigned action = 0; action < S::BUTTON_ACTION_COUNT; ++action) {
          if (!buttonActionAvailable(action, caps)) continue;
          const auto option = buttonActionOption(action, caps);
          assert(option == count++ && buttonActionAtOption(option, caps) == action);
          power.setEnumSelectedIndex(option);
          assert(SETTINGS.btnDoublePower == action && power.getEnumSelectedIndex() == option);
          assert(!power.getDisplayValue().empty());
          assert(JsonSettingsIO::saveSettings(SETTINGS, "settings"));
          const auto serialized = Storage.files.at("settings");
          JsonDocument document;
          assert(!deserializeJson(document, serialized));
          assert(document["btnDoublePower"].as<unsigned>() == action);
          SETTINGS.btnDoublePower = defaultPower;
          bool resave = false;
          assert(JsonSettingsIO::loadSettings(SETTINGS, serialized.c_str(), &resave));
          assert(SETTINGS.btnDoublePower == action);
        }
        assert(count == power.getEnumOptionCount());
        auto forward = row("btnDoublePageForward");
        forward.setEnumSelectedIndex(buttonActionOption(S::BTN_TOGGLE_STATUS_BAR, caps));
        assert(SETTINGS.btnDoublePageForward == S::BTN_TOGGLE_STATUS_BAR);
        assert(forward.getDisplayValue() == "STR_BTN_ACT_TOGGLE_STATUS_BAR");
        assert(JsonSettingsIO::saveSettings(SETTINGS, "forward"));
        SETTINGS.btnDoublePageForward = S::BTN_DEFAULT;
        assert(JsonSettingsIO::loadSettings(SETTINGS, Storage.files.at("forward").c_str()));
        assert(SETTINGS.btnDoublePageForward == S::BTN_TOGGLE_STATUS_BAR);
        power.setEnumSelectedIndex(buttonActionOption(S::BTN_TOGGLE_SETTINGS, caps));
        assert(SETTINGS.btnDoublePower == S::BTN_TOGGLE_SETTINGS);
        assert(power.getDisplayValue() == "STR_BTN_ACT_TOGGLE_SETTINGS");
        if (usb) {
          previous.setEnumSelectedIndex(buttonActionOption(S::BTN_USB_DRIVE, caps));
          assert(SETTINGS.btnDoublePageBack == S::BTN_USB_DRIVE);
          assert(previous.getDisplayValue() == "STR_USB_DRIVE");
        }
        SETTINGS.btnDoublePower = S::BTN_LIGHT_WARMER;
        SETTINGS.btnDoublePageBack = S::BTN_USB_DRIVE;
        S::dropUnsupportedActions(SETTINGS, light, warm, usb);
        assert(SETTINGS.btnDoublePower == (light && warm ? S::BTN_LIGHT_WARMER : S::BTN_DEFAULT));
        assert(SETTINGS.btnDoublePageBack == (usb ? S::BTN_USB_DRIVE : S::BTN_DEFAULT));
        SETTINGS.btnDoublePower = defaultPower;
        SETTINGS.btnDoublePageBack = defaultPrevious;
        assert(JsonSettingsIO::loadSettings(SETTINGS, "{\"btnDoublePower\":255,\"btnDoublePageBack\":255}"));
        assert(SETTINGS.btnDoublePower == defaultPower && SETTINGS.btnDoublePageBack == defaultPrevious);
      }
  // The actual main.cpp dispatch resolves the requested pair. Power hold stays default.
  SETTINGS.btnDoublePower = defaultPower;
  SETTINGS.btnDoublePageBack = defaultPrevious;
  SETTINGS.btnDoublePower = S::BTN_TOGGLE_SETTINGS;
  SETTINGS.btnDoublePageBack = S::BTN_USB_DRIVE;
  assert(mappedAction({B::Power, P::Double}) == S::BTN_TOGGLE_SETTINGS);
  assert(mappedAction({B::PageBack, P::Double}) == S::BTN_USB_DRIVE);
  assert(mappedAction({B::Power, P::Long}) == S::BTN_DEFAULT);
  assert(mappedAction({B::Up, P::Double}) == S::BTN_DEFAULT);  // alias cannot launch USB twice
  assert(!S::isReaderScopedAction(S::BTN_TOGGLE_SETTINGS));
  assert(!S::isReaderScopedAction(S::BTN_USB_DRIVE));

  checkTransitionReplay();

  MappedInputManager input;
  ButtonEventManager buttons(input);
  buttons.setReaderOnTopQuery([] { return false; });
  assert(buttons.hasDoubleAction(B::Power));
  assert(buttons.hasDoubleAction(B::PageBack) && buttons.hasDoubleAction(B::Up));
  // One normal Previous press remains one Short per alias after the 300ms window.
  input.edge(B::PageBack, true, 100);
  input.edge(B::PageBack, false, 140);
  hostNow = 140;
  assert(events(buttons).empty());
  hostNow = 439;
  assert(events(buttons).empty());
  hostNow = 440;
  auto single = events(buttons);
  assert(single.size() == 2 && single[0].type == P::Short && single[1].type == P::Short);
  // Double Power, including all sampler edges delivered after a slow display refresh.
  input.edge(B::Power, true, 500);
  input.edge(B::Power, false, 550);
  input.edge(B::Power, true, 650);
  input.edge(B::Power, false, 690);
  hostNow = 1400;
  auto powerEvents = events(buttons);
  assert(powerEvents.size() == 1 && powerEvents[0].type == P::Double);
  assert(mappedAction(powerEvents[0]) == S::BTN_TOGGLE_SETTINGS);
  // Double Previous emits both logical aliases, but exactly one global USB action.
  input.edge(B::PageBack, true, 1500);
  input.edge(B::PageBack, false, 1550);
  input.edge(B::PageBack, true, 1650);
  input.edge(B::PageBack, false, 1690);
  hostNow = 2000;
  auto previousEvents = events(buttons);
  assert(previousEvents.size() == 2);
  unsigned usbActions = 0;
  for (const auto& event : previousEvents) {
    assert(event.type == P::Double);
    usbActions += mappedAction(event) == S::BTN_USB_DRIVE;
  }
  assert(usbActions == 1);
  // A transition drains a pending single; it cannot leak into Settings/USB.
  input.edge(B::Power, true, 2100);
  input.edge(B::Power, false, 2150);
  hostNow = 2150;
  assert(events(buttons).empty());
  buttons.drain();
  hostNow = 2600;
  assert(events(buttons).empty());
  SETTINGS.btnDoublePageForward = S::BTN_TOGGLE_STATUS_BAR;
  assert(S::isReaderScopedAction(S::BTN_TOGGLE_STATUS_BAR));
  assert(mappedAction({B::PageForward, P::Double}) == S::BTN_TOGGLE_STATUS_BAR);
  assert(mappedAction({B::Down, P::Double}) == S::BTN_DEFAULT);
  // In a book, physical Down/PageForward aliases deliver exactly one action.
  buttons.setReaderOnTopQuery([] { return true; });
  input.edge(B::PageForward, true, 3000);
  input.edge(B::PageForward, false, 3040);
  input.edge(B::PageForward, true, 3140);
  input.edge(B::PageForward, false, 3180);
  hostNow = 3800;
  buttons.update();
  DispatchManagerProbe reader;
  dispatchShortcuts(buttons, reader);
  assert(reader.statusBarCalls == 1 && reader.pendingAction == PendingAction::None);
  buttons.drain();
  // Outside a reader, the short remains immediate and no toggle is dispatched.
  buttons.setReaderOnTopQuery([] { return false; });
  assert(!buttons.hasDoubleAction(B::Down) && !buttons.hasDoubleAction(B::PageForward));
  input.edge(B::PageForward, true, 4000);
  input.edge(B::PageForward, false, 4040);
  hostNow = 4040;
  const auto forwardSingle = events(buttons);
  assert(forwardSingle.size() == 2);
  for (const auto& event : forwardSingle) assert(event.type == P::Short);
  DispatchManagerProbe menu;
  menu.readerOnTop = false;
  buttons.pushEventFront({B::PageForward, P::Double});
  dispatchShortcuts(buttons, menu);
  assert(menu.statusBarCalls == 0);
  // Preserve upstream's per-activity opt-in: only Power pages a dictionary definition.
  buttons.drain();
  SETTINGS.btnDoublePower = S::BTN_PAGE_FORWARD;
  SETTINGS.btnDoublePageForward = S::BTN_PAGE_FORWARD;
  DispatchManagerProbe dictionary;
  dictionary.readerOnTop = false;
  dictionary.dictionaryOnTop = true;
  buttons.pushEventFront({B::PageForward, P::Double});
  buttons.pushEventFront({B::Power, P::Double});
  dispatchShortcuts(buttons, dictionary);
  assert(dictionary.pageCalls == 1 && dictionary.statusBarCalls == 0);
  ButtonEventManager::ButtonEvent untouched;
  assert(buttons.consumeEvent(untouched) && untouched.button == B::PageForward);
  assert(!buttons.consumeEvent(untouched));
  // Existing reader-only defaults still avoid double-click delay on other screens.
  assert(!buttons.hasDoubleAction(B::Left));
  std::cout << "PASS: stable action IDs, capability filtering, real settings JSON/UI, production dispatch and double "
               "presses\n";
}
