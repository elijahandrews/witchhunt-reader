#!/usr/bin/env python3
"""Compile the production USB Drive startup method with recorded hardware operations."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
a=p.parse_args(); r=a.repo.resolve(); out=a.out.resolve(); out.mkdir(parents=True,exist_ok=True)
s=(r/'lib/hal/HalStorage.cpp').read_text()
start='bool HalStorage::beginUsbDrive() {'
body=start+s.split(start,1)[1].split('bool HalStorage::disconnectUsbDriveHost()',1)[0]
probe=r'''#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#define LOG_ERR(...)
#define LOG_INF(...)
std::vector<std::string> events;
bool pads=true;
bool usb_serial_jtag_ll_phy_is_pad_enabled() { return pads; }
void usb_serial_jtag_ll_phy_enable_pad(bool on) { pads=on; events.push_back(on?"pad-on":"pad-off"); }
void delay(unsigned ms) { assert(ms==250); events.push_back("delay"); }
struct StorageLock {};
struct SDProbe {
 bool mounted=true;
 int device=1;
 int* detachFilesystemForRawAccess() { events.push_back("detach"); return mounted?&device:nullptr; }
 bool begin() { events.push_back("remount"); return true; }
} SDCard;
struct MSCProbe {
 bool running=false, ok=true;
 bool active() const { return running; }
 bool begin(int* dev) { assert(dev); events.push_back("msc"); running=ok; return ok; }
} usbMassStorage;
struct HalStorage { bool beginUsbDrive(); } storage;
'''+body+r'''
void reset() { events.clear(); pads=true; SDCard.mounted=true; usbMassStorage.running=false; usbMassStorage.ok=true; }
int main() {
#if FREEINK_CAP_USB_MSC
 reset(); usbMassStorage.running=true; assert(!storage.beginUsbDrive()); assert(events.empty());
 reset(); SDCard.mounted=false; assert(!storage.beginUsbDrive()); assert(events==std::vector<std::string>{"detach"});
 reset(); assert(storage.beginUsbDrive());
#if CONFIG_IDF_TARGET_ESP32S3 && ARDUINO_USB_MODE
 assert((events==std::vector<std::string>{"detach","pad-off","delay","msc"}));
#else
 assert((events==std::vector<std::string>{"detach","msc"}));
#endif
 reset(); usbMassStorage.ok=false; assert(!storage.beginUsbDrive());
#if CONFIG_IDF_TARGET_ESP32S3 && ARDUINO_USB_MODE
 assert((events==std::vector<std::string>{"detach","pad-off","delay","msc","pad-on","remount"}));
 assert(pads);
 reset(); pads=false; usbMassStorage.ok=false; assert(!storage.beginUsbDrive()); assert(!pads);
#else
 assert((events==std::vector<std::string>{"detach","msc","remount"}));
#endif
#else
 reset(); assert(!storage.beginUsbDrive()); assert(events.empty());
#endif
 std::cout<<"PASS: production USB startup ordering, active-session protection, missing-card and startup-failure paths\n";
}
'''
src=out/'usb.cpp'; src.write_text(probe)
for cap,s3,hw in [(1,1,1),(1,1,0),(1,0,1),(0,0,0)]:
 exe=out/f'usb-{cap}-{s3}-{hw}'
 subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
  f'-DFREEINK_CAP_USB_MSC={cap}',f'-DCONFIG_IDF_TARGET_ESP32S3={s3}',f'-DARDUINO_USB_MODE={hw}',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
