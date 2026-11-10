#pragma once

#include <HalStorage.h>
#include <I18n.h>

#include "activities/Activity.h"

class BookSyncActivity final : public Activity {
 public:
  BookSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BookSyncActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void setReturnToFileTransfer(std::string bookPath, bool restart) {
    returnBookPath = std::move(bookPath);
    restartOnLeave = restart;
  }
  bool preventAutoSleep() override { return busy; }

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
  std::string returnBookPath;
  bool restartOnLeave = false;
  bool actionTapped();
  void leave();
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
