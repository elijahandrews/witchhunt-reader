#pragma once
#include <cstdlib>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_DEFAULT 2
inline size_t heap_caps_get_largest_free_block(int) { return 1000000; }

inline size_t esp_get_free_heap_size() { return 1000000; }
