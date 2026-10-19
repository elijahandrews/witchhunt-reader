#!/usr/bin/env python3
"""Exercise production shortcut rows, JSON, action dispatch and button classifier with host IO shims."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess


def between(text, start, end):
    assert text.count(start) == 1 and text.count(end) == 1, (start, end)
    return text.split(start, 1)[1].split(end, 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', action='store_true', help='prove the old pending-transition input gate fails')
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    shim = out / 'shims'
    shim.mkdir(exist_ok=True)
    # Reuse the existing in-memory SD/String shim, not production settings logic.
    spec = importlib.util.spec_from_file_location('recent_shims', repo / 'test/recent_book_overrides/run.py')
    recent = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recent)
    for name in ('HalStorage.h', 'Logging.h'):
        (shim / name).write_text(recent.SHIMS[name])
    keys = re.findall(r'^(STR_[A-Z0-9_]+):', (repo / 'lib/I18n/translations/english.yaml').read_text(), re.M)
    assert len(keys) == len(set(keys))
    (shim / 'I18n.h').write_text('#pragma once\n#include <cstdint>\n#include <string>\n'
        'enum class StrId { '+','.join(keys)+' };\nstruct HostI18n { const char* get(StrId id) const {\n'
        'static const char* labels[] = {'+','.join('"'+key+'"' for key in keys)+'};\n'
        'return labels[static_cast<unsigned>(id)]; } };\ninline HostI18n I18N;\n#define tr(k) I18N.get(StrId::k)\n')
    (shim / 'Arduino.h').write_text('#pragma once\n#include <cstdint>\ninline unsigned long hostNow = 0;\ninline unsigned long millis() { return hostNow; }\n')
    (shim / 'MappedInputManager.h').write_text(r'''#pragma once
#include <Arduino.h>
#include <deque>
struct HalGPIO { struct ButtonEdge { uint8_t button; bool pressed; unsigned long timeMs; }; };
class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  std::deque<HalGPIO::ButtonEdge> edges;
  bool held[7]{};
  uint8_t rawIndex(Button b) const {
    switch (b) { case Button::PageBack: return 4; case Button::PageForward: return 5;
      default: return static_cast<uint8_t>(b); }
  }
  bool isPressed(Button b) const { return held[rawIndex(b)]; }
  bool popRawEdge(HalGPIO::ButtonEdge& edge) {
    if (edges.empty()) return false; edge=edges.front(); edges.pop_front(); return true;
  }
  void flushRawEdges() { edges.clear(); }
  void flushTouchEvents() {}
  void edge(Button b, bool down, unsigned long at) { held[rawIndex(b)]=down; edges.push_back({rawIndex(b),down,at}); }
};
''')
    # Copied verbatim to let quoted MappedInputManager.h resolve to the IO shim.
    for name in ('ButtonEventManager.h', 'ButtonEventManager.cpp'):
        (shim / name).write_bytes((repo / 'src' / name).read_bytes())
    (shim / 'components').mkdir(exist_ok=True)
    (shim / 'components/UITheme.h').write_text('#pragma once\n#include <string>\nstruct UITheme { static std::string makeSeparatorTitle(std::string s) { return s; } };\n')

    settings = (repo / 'src/SettingsList.h').read_text()
    helper = 'inline ButtonActionCapabilities buttonActionCapabilities()' + between(settings,
        'inline ButtonActionCapabilities buttonActionCapabilities()', 'inline void buildSettings(const RowSink& emit) {')
    labels = between(settings, 'inline void buildSettings(const RowSink& emit) {', '  // --- Display ---')
    buttons = '  // --- Button Actions' + between(settings, '  // --- Button Actions', '  // --- Gesture actions (reader only) ---')
    visibility = between(settings, '  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---', '  emit(SettingInfo::Enum(StrId::STR_UPPER_PROGRESS_BAR,')
    settings_cpp = (repo / 'src/CrossPointSettings.cpp').read_text()
    methods = 'void CrossPointSettings::normalizeDependentSettings' + between(settings_cpp,
        'void CrossPointSettings::normalizeDependentSettings', '#include <Preferences.h>')
    json = between((repo / 'src/JsonSettingsIO.cpp').read_text(), '// ---- CrossPointSettings ----', '// ---- KOReaderCredentialStore ----')
    main_cpp = (repo / 'src/main.cpp').read_text()
    action_for = 'auto actionFor = ' + between(main_cpp, 'auto actionFor = ', '    // Reader-scoped actions are only meaningful')
    dispatch_cases = '        case BA::BTN_TOGGLE_SETTINGS:' + between(main_cpp,
        '        case BA::BTN_TOGGLE_SETTINGS:', '        case BA::BTN_GO_HOME:')
    dispatch_cases += '        case BA::BTN_TOGGLE_STATUS_BAR:' + between(main_cpp,
        '        case BA::BTN_TOGGLE_STATUS_BAR:', '        case BA::BTN_TOGGLE_BIONIC_READING:')
    dispatch_loop = '    ButtonEventManager::ButtonEvent ev;' + between(main_cpp,
        '    ButtonEventManager::ButtonEvent ev;', '    // Gestures. In the reader every gesture is live;')
    manager = (repo / 'src/activities/ActivityManager.cpp').read_text()
    gate = re.search(r'if \((!drainInput && currentActivity[^\n]*)\) \{', manager).group(1)
    if args.negative_control:
        assert 'pendingAction == PendingAction::None' in gate
        gate = '!drainInput && currentActivity'
    replay = r'''enum class PendingAction { None, Push, Pop, Replace };
struct DispatchManagerProbe {
  int current = 1;
  int* currentActivity = &current;
  bool drainInput = false;
  PendingAction pendingAction = PendingAction::None;
  int destination = 0;
  bool readerOnTop = true;
  int statusBarCalls = 0;
  bool currentIsReaderActivity() const { return readerOnTop; }
  void dispatchButtonAction(CrossPointSettings::BUTTON_ACTION a) {
    assert(a == CrossPointSettings::BTN_TOGGLE_STATUS_BAR); ++statusBarCalls;
  }
  void toggleSettings() { pendingAction=PendingAction::Replace; destination=1; }
  void goToUsbDrive() { pendingAction=PendingAction::Replace; destination=2; }
  bool outgoingLoopRuns() const { return ''' + gate + r'''; }
};
void dispatchShortcuts(ButtonEventManager& buttonEventManager, DispatchManagerProbe& activityManager) {
  using BA=CrossPointSettings::BUTTON_ACTION;
  const auto isReaderScopedAction=[](uint8_t a) { return CrossPointSettings::isReaderScopedAction(a); };
  const auto actionFor=[](const ButtonEventManager::ButtonEvent& e) { return mappedAction(e); };
  const auto runAction=[&](BA action) { switch(action) {
''' + dispatch_cases + r''' default: break; } return true; };
''' + dispatch_loop + r'''
}
void checkTransitionReplay() {
  using B=MappedInputManager::Button;
  using P=ButtonEventManager::PressType;
  for (B key : {B::Power,B::PageBack}) {
    MappedInputManager input;
    ButtonEventManager buttons(input);
    DispatchManagerProbe manager;
    // The default Confirm was queued before the shortcut. drain() cannot erase
    // a copy main.cpp already placed in its local defaultEvents vector.
    buttons.pushEventFront({key,P::Double});
    buttons.pushEventFront({B::Confirm,P::Short});
    dispatchShortcuts(buttons,manager);
    assert(manager.destination==(key==B::Power ? 1 : 2));
    bool oldScreenActivated=false;
    if (manager.outgoingLoopRuns()) {
      ButtonEventManager::ButtonEvent event;
      while (buttons.consumeEvent(event)) {
        if (event.button==B::Confirm) { oldScreenActivated=true; manager.destination=99; }
      }
    }
    assert(!oldScreenActivated && manager.destination==(key==B::Power ? 1 : 2));
  }
  DispatchManagerProbe manager;
  assert(manager.outgoingLoopRuns());
  for (auto action : {PendingAction::Push,PendingAction::Pop,PendingAction::Replace}) {
    manager.pendingAction=action;
    assert(!manager.outgoingLoopRuns());
  }
}
'''
    generated = out / 'ShortcutProbe.cpp'
    generated.write_text('''#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <vector>
#include <ArduinoJson.h>
#include <Logging.h>
#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "JsonSettingsIO.h"
#include "activities/settings/SettingInfo.h"
#include "util/ButtonActionOptions.h"
inline bool hostUsb = true;
#define FREEINK_CAP_USB_MSC hostUsb
#define CP_TOUCH_UI 0
struct HostLight { bool light=true, warm=true; bool present() const { return light; }
  bool hasColorTemperature() const { return warm; } } Frontlight;
namespace obfuscation {
inline std::string obfuscateToBase64(const char* p) { return p; }
inline std::string deobfuscateFromBase64(const char* p, size_t, bool* ok, bool* large) { *ok=true; *large=false; return p; }
}
CrossPointSettings CrossPointSettings::instance;
''' + methods + '\n' + helper + '\n'
        + 'template<class Visitor> void forEachSetting(Visitor emit) {\n' + labels + buttons + visibility + '\n}\n'
        + json + '\n'
        + 'uint8_t mappedAction(const ButtonEventManager::ButtonEvent& event) { using B=MappedInputManager::Button; using BA=CrossPointSettings::BUTTON_ACTION;\n'
        + action_for + '\nreturn actionFor(event); }\n'
        + replay + '\n' + (Path(__file__).with_name('ShortcutTest.cpp')).read_text())
    binary = out / 'shortcut-test'
    command = ['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra']
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command += ['-I'+str(shim), '-I'+str(repo/'src'), '-I'+str(repo/'.pio/libdeps/x4pro/ArduinoJson/src'),
                str(generated), str(shim/'ButtonEventManager.cpp'), str(repo/'src/activities/settings/SettingInfo.cpp'), '-o', str(binary)]
    subprocess.run(command, check=True)
    if args.negative_control:
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode != 0 and 'oldScreenActivated' in result.stderr, result.stderr
        print('PASS: negative control rejected old outgoing-input gate')
    else:
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
