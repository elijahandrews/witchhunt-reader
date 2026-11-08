#!/usr/bin/env python3
"""Check the real setting row and generic JSON save/load loops with ArduinoJson."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--arduino-json', type=Path, help='ArduinoJson src directory (defaults to x4pro libdeps)')
parser.add_argument('--sanitize', action='store_true', help='Enable ASan/UBSan when supported by the host compiler')
args = parser.parse_args()
repo, out = args.repo.resolve(), args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
json_include = args.arduino_json or repo / '.pio/libdeps/x4pro/ArduinoJson/src'
with (out / 'i18n.log').open('w') as log:
    subprocess.run([sys.executable, str(repo / 'scripts/gen_i18n.py'),
                    str(repo / 'lib/I18n/translations'), str(out)], stdout=log, stderr=log, check=True)
(out / 'HalStorage.h').write_text('#pragma once\nclass FsFile;\n')
(out / 'I18n.h').write_text('''#pragma once
#include "I18nKeys.h"
class I18n { public: static I18n& getInstance(); const char* get(StrId) const; };
#define I18N I18n::getInstance()
''')
settings = (repo / 'src/SettingsList.h').read_text()
start = settings.index('  emit(SettingInfo::Toggle(StrId::STR_TAP_STATUS_BAR,')
row = settings[start:settings.index(';', start) + 1]
json_io = (repo / 'src/JsonSettingsIO.cpp').read_text()

def loop_in(function):
    start = json_io.index('  forEachSetting([&](const SettingInfo& info) {', json_io.index(function))
    return json_io[start:json_io.index('\n  });', start) + len('\n  });')]

source = r'''#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include "activities/settings/SettingInfo.h"
#define LOG_ERR(...) ((void)0)
CrossPointSettings CrossPointSettings::instance;
// The string-only branches are compiled but cannot run for this toggle row.
namespace obfuscation {
std::string obfuscateToBase64(const char*) { assert(false); return {}; }
std::string deobfuscateFromBase64(const char*,size_t,bool*,bool*) { assert(false); return {}; }
}
template<typename Emit> void forEachSetting(Emit emit) {
''' + row + r'''
}
std::string save(const CrossPointSettings& s) {
 JsonDocument doc;
''' + loop_in('bool JsonSettingsIO::saveSettings(') + r'''
 std::string result; serializeJson(doc,result); return result;
}
void load(CrossPointSettings& s,const char* json) {
 JsonDocument doc; assert(!deserializeJson(doc,json));
 bool resave=false; bool* needsResave=&resave;
 auto clamp=[](uint8_t value,uint8_t max,uint8_t fallback) -> uint8_t { return value<max?value:fallback; };
 // Version migration is irrelevant to this non-gesture preference.
 const bool staleGestureDefaults=true;
 const auto isGestureKey=[](const char*) { return false; };
''' + loop_in('bool JsonSettingsIO::loadSettings(') + r'''
}
int main() {
 auto& s=SETTINGS;
 const auto compiledDefault=s.tapToToggleStatusBar;
 assert(compiledDefault==1);
 int count=0;
 forEachSetting([&](const SettingInfo& info) {
  ++count;
  assert(info.type==SettingType::TOGGLE && info.valuePtr==&CrossPointSettings::tapToToggleStatusBar);
  assert(info.category==StrId::STR_CAT_CONTROLS && info.requiredCapability==SettingRequires::TouchPanel);
  assert(std::strcmp(info.key,"tapToToggleStatusBar")==0);
 });
 assert(count==1);
 // Old files lacking the key retain the member initializer, including files with unrelated values.
 for(const char* json:{"{}", "{\"statusBarVisible\":0}", "{\"tapToToggleStatusBar\":null}",
                       "{\"tapToToggleStatusBar\":2}", "{\"tapToToggleStatusBar\":255}"}) {
  s.tapToToggleStatusBar=compiledDefault; load(s,json); assert(s.tapToToggleStatusBar==1);
 }
 // A saved off remains off across a fresh load; visible/hidden is a separate preference.
 for(int value:{0,1}) {
  s.tapToToggleStatusBar=value; const auto json=save(s);
  JsonDocument written; assert(!deserializeJson(written,json));
  assert(written["tapToToggleStatusBar"].as<int>()==value);
  s.tapToToggleStatusBar=compiledDefault; s.statusBarVisible=!value;
  load(s,json.c_str()); assert(s.tapToToggleStatusBar==value && s.statusBarVisible==!value);
 }
 std::cout << "PASS: actual menu row and JSON loops preserve default-on, explicit off/on, invalid-value fallback and visibility independence\n";
}
'''
source_path, executable = out / 'settings.cpp', out / 'settings-test'
source_path.write_text(source)
sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
subprocess.run(['c++', '-std=c++20', '-O1', '-g'] + sanitizers + [
                '-I' + str(out), '-I' + str(repo / 'src'), '-I' + str(json_include),
                str(source_path), '-o', str(executable)], check=True)
subprocess.run([str(executable)], check=True)
