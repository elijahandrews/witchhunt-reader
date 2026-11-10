#pragma once
namespace trustedtime {
inline long trustedNow() { return 1800000000; }
inline bool syncNow(unsigned) { return true; }
}  // namespace trustedtime
