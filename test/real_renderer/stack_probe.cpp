// A deliberately small host thread complements target compiler stack reports.
// Do not run this probe with ASan: its stack instrumentation changes the budget.
#include <pthread.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "OutlineFontFace.h"

namespace {
constexpr size_t kThreadStack = 32 * 1024;
constexpr size_t kCallerReserve = 8 * 1024;

struct Probe {
  std::vector<uint8_t> font;
  size_t rasters = 0;
  size_t inkBytes = 0;
  uint32_t checksum = 0;
  uintptr_t stackLow = 0, stackHigh = 0;
  size_t maximumObservedStack = 0, heapUsed = 0, actualStackBytes = 0;
};

void checkStack(Probe& probe) {
  volatile uint8_t marker = 0;
  const auto address = reinterpret_cast<uintptr_t>(&marker);
  if (address < probe.stackLow || address >= probe.stackHigh) {
    std::fprintf(stderr, "backend callback exceeded requested stack budget: observed=%zu allowed=%zu\n",
                 size_t(probe.stackHigh - address), size_t(probe.stackHigh - probe.stackLow));
    std::abort();
  }
  const size_t used = probe.stackHigh - address;
  if (used > probe.maximumObservedStack) probe.maximumObservedStack = used;
}

bool claimMemory(void* context, size_t bytes) {
  auto& probe = *static_cast<Probe*>(context);
  checkStack(probe);
  if (bytes > 1024 * 1024 - probe.heapUsed) return false;
  probe.heapUsed += bytes;
  return true;
}

void releaseMemory(void* context, size_t bytes) {
  auto& probe = *static_cast<Probe*>(context);
  checkStack(probe);
  assert(bytes <= probe.heapUsed);
  probe.heapUsed -= bytes;
}

size_t readFont(void* context, uint32_t offset, uint8_t* buffer, size_t count) {
  checkStack(*static_cast<Probe*>(context));
  const auto& source = static_cast<Probe*>(context)->font;
  if (offset > source.size() || count > source.size() - offset) return 0;
  std::memcpy(buffer, source.data() + offset, count);
  return count;
}

// Keep this real stack reservation alive across the entire backend call chain.
// It represents application callers; it is not a calibration of ESP ABI usage.
__attribute__((noinline)) void* run(void* context) {
  volatile uint8_t callerReserve[kCallerReserve];
  for (size_t n = 0; n < kCallerReserve; ++n) callerReserve[n] = uint8_t(n);
  auto& probe = *static_cast<Probe*>(context);
#if defined(__APPLE__)
  probe.stackHigh = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  probe.stackLow = probe.stackHigh - pthread_get_stacksize_np(pthread_self());
#else
  pthread_attr_t actual;
  assert(!pthread_getattr_np(pthread_self(), &actual));
  void* stackBase = nullptr;
  size_t stackSize = 0;
  assert(!pthread_attr_getstack(&actual, &stackBase, &stackSize));
  assert(!pthread_attr_destroy(&actual));
  probe.stackLow = reinterpret_cast<uintptr_t>(stackBase);
  probe.stackHigh = probe.stackLow + stackSize;
#endif
  probe.actualStackBytes = probe.stackHigh - probe.stackLow;
  // macOS rounds this allocation up (45,056 bytes for a requested 32,768).
  // Enforce the requested budget explicitly instead of trusting guard pages.
  assert(probe.actualStackBytes >= kThreadStack);
  probe.stackLow = probe.stackHigh - kThreadStack;
  checkStack(probe);
  for (auto hinting : {OutlineFontFace::Hinting::Native, OutlineFontFace::Hinting::Light}) {
    for (auto policy : {OutlineFontFace::RasterPolicy::FinalSize, OutlineFontFace::RasterPolicy::Oversample2x}) {
      OutlineFontFace face;
      OutlineFontFace::Options options;
      options.hinting = hinting;
      options.rasterPolicy = policy;
      options.applyVariations = false;
      options.loadLayoutTables = false;
      options.allocationContext = &probe;
      options.claimMemory = claimMemory;
      options.releaseMemory = releaseMemory;
      assert(face.openStream(readFont, &probe, uint32_t(probe.font.size()), options));
      for (float scale : {0.3f, 0.5f, 0.9f}) {
        static constexpr uint32_t samples[] = {'H', 'N', 'T', 'A',    'V',    'T',    'o',   'i',
                                               'l', 'j', 'f', 0x00E9, 0x00C5, 0x00C4, 0x0301};
        for (uint32_t cp : samples) {
          const auto gid = face.glyphId(cp);
          if (!gid) continue;
          // Exercise the stored/native bitmap route as well as final rasters.
          assert(face.metrics(gid));
          size_t legacyBytes = 0;
          assert(face.bitmap(gid, &legacyBytes) || legacyBytes == 0);
          for (int phase : {-8, 0, 7}) {
            EpdRasterGlyph raster;
            assert(face.raster(gid, scale, int8_t(phase), &raster));
            assert(raster.metrics && (!raster.bytes || raster.bitmap));
            for (size_t n = 0; n < raster.bytes; ++n) probe.checksum = probe.checksum * 31 + raster.bitmap[n];
            probe.inkBytes += raster.bytes;
            ++probe.rasters;
          }
        }
      }
    }
  }
  for (size_t n = 0; n < kCallerReserve; ++n) assert(callerReserve[n] == uint8_t(n));
  assert(probe.rasters >= 300 && probe.inkBytes && !probe.heapUsed);
  return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc > 1);
  for (int n = 1; n < argc; ++n) {
    Probe probe;
    FILE* file = std::fopen(argv[n], "rb");
    assert(file && std::fseek(file, 0, SEEK_END) == 0);
    const auto length = std::ftell(file);
    assert(length > 0 && uint64_t(length) <= UINT32_MAX);
    probe.font.resize(size_t(length));
    std::rewind(file);
    assert(std::fread(probe.font.data(), 1, probe.font.size(), file) == probe.font.size());
    std::fclose(file);
    pthread_attr_t attributes;
    assert(!pthread_attr_init(&attributes));
    assert(!pthread_attr_setstacksize(&attributes, kThreadStack));
    pthread_t thread;
    assert(!pthread_create(&thread, &attributes, run, &probe));
    assert(!pthread_attr_destroy(&attributes));
    assert(!pthread_join(thread, nullptr));
    std::printf(
        "font %d: %zu rasters, %zu bitmap bytes, checksum %08x; stack=%zu caller-reserve=%zu observed-at-callbacks=%zu "
        "actual-pthread=%zu PASS\n",
        n, probe.rasters, probe.inkBytes, probe.checksum, kThreadStack, kCallerReserve, probe.maximumObservedStack,
        probe.actualStackBytes);
  }
}
