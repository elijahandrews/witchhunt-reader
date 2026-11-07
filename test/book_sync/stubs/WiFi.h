#pragma once
constexpr int WL_CONNECTED = 1, WIFI_MODE_NULL = 0;
struct Wifi {
  int status() const { return WL_CONNECTED; }
  int getMode() const { return WIFI_MODE_NULL; }
  void disconnect(bool) {}
};
inline Wifi WiFi;
