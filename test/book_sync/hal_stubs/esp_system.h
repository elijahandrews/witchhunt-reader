#pragma once
constexpr int ESP_RST_SW = 1;
inline int fakeResetReason = 0;
inline int esp_reset_reason() { return fakeResetReason; }
