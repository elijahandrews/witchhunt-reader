#pragma once

#include <HalStorage.h>
#include <I18n.h>

#include "activities/UiListActivity.h"

class BookSyncActivity final : public UiListActivity {
 public:
  BookSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BookSyncActivity() override;
  void onEnter() override;
  void onExit() override;
  bool usesWifi() const override { return true; }
  bool preventAutoSleep() override { return busy; }
  void appendDebugState(std::string& out) const override;

 private:
  struct Work;
  // Cold-path work buffers are allocated once, not on the small loop task stack or permanently
  // in static RAM. Reused for every manifest line, SD hash and HTTP transfer in this activity.
  std::unique_ptr<Work> work;
  bool busy = true, cancelled = false;
  StrId status;
  unsigned added = 0, skipped = 0, conflicts = 0;
  unsigned checkedShown = 0, hashedMiBShown = 0;
  unsigned long lastPaint = 0, lastYield = 0;
  struct Metrics {
    uint32_t totalMs = 0, wifiMs = 0, manifestMs = 0, scanMs = 0, downloadMs = 0, verifyMs = 0;
    uint32_t cacheMs = 0, freeSpaceMs = 0, paintWaitMs = 0, readCalls = 0;
    uint64_t readUs = 0, hashUs = 0, localBytes = 0, verifyBytes = 0;
    bool cacheHit = false;
  } metrics;
  unsigned long startedAt = 0;
  int listCount() const override { return 0; }
  const char* headerTitle() const override;
  const char* footerConfirmLabel() const override { return ""; }
  const char* footerBackLabel() const override;
  void activateIndex(int) override {}
  void buildScreen(UiScreen& screen) override;
  void drawChrome() override;
  void connected(bool success);
  bool sync();
  bool pump();
  void show(StrId message);
  enum class HashKind { Local, Verify, Index };
  bool hashFile(FsFile& file, uint8_t* digest, HashKind kind = HashKind::Local);
  bool indexLocal(const std::string& path, FsFile& index, unsigned depth = 0);
  bool containsHash(FsFile& index, const uint8_t* digest, bool& found);
  bool loadIndex(FsFile& index);
  void saveIndex(FsFile& index);
  void saveMetrics(bool success);
};
