#!/usr/bin/env python3
"""Exercise production exclusive main-loop and USB activity input dispatch."""
import argparse
from pathlib import Path
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--out', type=Path, required=True)
p.add_argument('--negative-control', action='store_true')
a = p.parse_args()
r = a.repo.resolve()
main = (r / 'src/main.cpp').read_text()
start = main.index('void loop() {')
branch = function(main[start:], '  if (activityManager.requiresExclusiveStorageLoop())')
prefix = main[start:main.index('  if (activityManager.requiresExclusiveStorageLoop())', start)]
loop = prefix + branch + '\n}\n'
if a.negative_control:
    assert '    mappedInputManager.update();' not in loop
    loop = loop.replace('    activityManager.loop();', '    mappedInputManager.update();\n    activityManager.loop();')
activity = function((r / 'src/activities/network/UsbDriveActivity.cpp').read_text(), 'void UsbDriveActivity::loop()')
header = (r / 'src/activities/network/UsbDriveActivity.h').read_text()
constants = '\n'.join(line for line in header.splitlines() if 'static constexpr unsigned long' in line)
input_update = function((r / 'src/MappedInputManager.h').read_text(), '  void update() const')
code = r'''#include <cassert>
#include <cstdio>
#include <cstdint>
#define LOG_INF(...)
#define LOG_ERR(...)
unsigned long now = 10;
unsigned long millis() { return now; }
void delay(unsigned long) {}
struct Gpio {
  unsigned accum = 0, snapshot = 0, updates = 0;
  void update() { ++updates; snapshot = accum; accum = 0; }
} gpio;
struct MappedInputManager {
  enum class Button { Back = 1, Power = 2 };
  // MAPPED_UPDATE
  bool wasPressed(Button b) { return (gpio.snapshot & static_cast<unsigned>(b)) != 0; }
  bool wasHomeGesture() { return (gpio.snapshot & 4) != 0; }
} mappedInputManager;
struct TouchExitButton { static bool tapped(MappedInputManager&) { return (gpio.snapshot & 8) != 0; } };
struct { void update() {} } buttonEventManager;
struct { void setPowerSaving(bool) {} } powerManager;
enum class UsbDriveState { WaitingForHost, Connected, IoError, Ejected, Disconnected, Unsupported };
struct {
  UsbDriveState next = UsbDriveState::WaitingForHost;
  unsigned disconnects = 0;
  UsbDriveState usbDriveState() { return next; }
  bool disconnectUsbDriveHost() { ++disconnects; return true; }
} Storage;
struct UsbDriveActivity {
  MappedInputManager& mappedInput = mappedInputManager;
  UsbDriveState state = UsbDriveState::WaitingForHost;
  bool startFailed = false, forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 10, startFailureStartedAt = 10, forcedDisconnectRequestedAt = 0;
  unsigned restarts = 0;
  // CONSTANTS
  void requestUpdate() {}
  bool isIoError() const { return state == UsbDriveState::IoError; }
  bool cableLooksGone() { return false; }
  void restartToHome() { ++restarts; }
  void loop();
} activity;
// ACTIVITY_LOOP
struct {
  bool requiresExclusiveStorageLoop() const { return true; }
  void loop() { activity.loop(); }
  bool preventAutoSleep() const { return activity.state == UsbDriveState::Connected; }
} activityManager;
// MAIN_LOOP
void reset(UsbDriveState state) {
  now = 10; gpio = {}; Storage = {}; Storage.next = state;
  activity.~UsbDriveActivity(); new (&activity) UsbDriveActivity;
  activity.state = state;
}
int main() {
  unsigned checks = 0;
  for (unsigned event : {1u, 2u, 4u, 8u, 3u}) {
    reset(UsbDriveState::WaitingForHost); gpio.accum = event; loop();
    assert(gpio.updates == 1 && activity.restarts == 1); ++checks;
    reset(UsbDriveState::Connected); gpio.accum = event; loop();
    assert(gpio.updates == 1 && activity.restarts == 0 && Storage.disconnects == 0); ++checks;
    reset(UsbDriveState::IoError); activity.startFailed = true; gpio.accum = event; loop();
    assert(gpio.updates == 1 && activity.restarts == 1); ++checks;
  }
  reset(UsbDriveState::WaitingForHost); loop(); assert(activity.restarts == 0); ++checks;
  now = activity.hostWaitStartedAt + activity.HOST_WAIT_TIMEOUT_MS - 1;
  loop(); assert(activity.restarts == 0); ++checks;
  ++now; loop(); assert(activity.restarts == 1); ++checks;
  reset(UsbDriveState::IoError); activity.startFailed = true;
  now = activity.startFailureStartedAt + activity.START_FAILURE_TIMEOUT_MS;
  loop(); assert(activity.restarts == 1); ++checks;
  for (auto state : {UsbDriveState::Ejected, UsbDriveState::Disconnected, UsbDriveState::Unsupported}) {
    reset(state); loop(); assert(activity.restarts == 1); ++checks;
  }
  // A held level produces no new edge: another loop must not repeat the press.
  reset(UsbDriveState::WaitingForHost); gpio.accum = 2; loop();
  activity.restarts = 0; loop(); assert(activity.restarts == 0); ++checks;
  std::printf("%u production USB input/timeout/connected-state checks passed\n", checks);
}
'''
code = '#include <initializer_list>\n#include <new>\n' + code
for marker, value in [('MAPPED_UPDATE', input_update), ('CONSTANTS', constants), ('ACTIVITY_LOOP', activity), ('MAIN_LOOP', loop)]:
    code = code.replace('// ' + marker, value)
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / 'probe.cpp'
source.write_text(code)
command = ['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(source), '-o', str(out / 'probe')]
subprocess.run(command, check=True)
result = subprocess.run([str(out / 'probe')], capture_output=True, text=True)
(out / 'run.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
if a.negative_control:
    assert result.returncode != 0, 'Duplicate input drain unexpectedly passed'
    print('Negative control rejected: duplicate input drain')
else:
    assert result.returncode == 0, 'Production USB input regression failed'
