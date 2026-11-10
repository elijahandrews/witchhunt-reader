#pragma once
namespace freeink {
enum class UsbMassStorageState { WaitingForHost, Connected, Accessed, Ejected, Disconnected, IoError, Idle };
class UsbMassStorage {
 public:
  bool begin(void*) { return true; }
  bool disconnectHost() { return true; }
  bool hostSuspended() { return false; }
  void end() {}
  UsbMassStorageState state() { return UsbMassStorageState::Connected; }
};
}  // namespace freeink
