#pragma once
constexpr int WL_CONNECTED = 1, WIFI_MODE_NULL = 0, WIFI_OFF = 0;
inline bool wifiConnected = true;
struct Wifi {
  int status() const { return wifiConnected ? WL_CONNECTED : WIFI_MODE_NULL; }
  int getMode() const { return WIFI_MODE_NULL; }
  void disconnect(bool) {}
  void mode(int) {}
};
inline Wifi WiFi;
