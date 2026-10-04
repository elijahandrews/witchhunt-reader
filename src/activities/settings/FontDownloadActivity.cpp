#include "FontDownloadActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/FontManifestReader.h"

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();

  // Free the heap the WiFi stack needs before it is brought up, not after -
  // association itself is the allocation-heavy step, well ahead of TLS. Matters
  // most here because SettingsActivity is still on the stack below us with its
  // per-category SettingInfo vectors resident, fragmenting the heap.
  // WifiSelectionActivity sets WIFI_STA itself, so no radio work happens here.
  trimMemoryForNetworkSession(renderer, "FONT");

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  // Always silentRestart on exit, regardless of WiFi state. Even if a deep
  // error path turned WiFi off, we still did expensive network/TLS work and
  // the heap is fragmented past the point a normal session can recover (see
  // [project-font-download-heap-stash]). A reboot here gives the next
  // activity a pristine heap.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestart();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  // Re-trim: the status screens rendered since onEnter can have repopulated the
  // font cache. Idempotent - the secondary buffer is already gone by now.
  trimMemoryForNetworkSession(renderer, "FONT");

  if (!fetchAndParseManifest()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
    previousActionCount_ = actionCount();
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  // Standalone manifest fetch: closes the TLS connection before the JSON
  // parse so the parser has full heap headroom. The Session is opened later
  // for the per-file download loop, on a heap that's been slimmed by
  // trimManifestForDownload().
  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  // Streamed off the card a block at a time: a whole-document parse of the
  // 24 KB manifest ran the X3 out of heap and aborted. styles[] in the JSON
  // is skipped -- see families_.
  FontManifestReader reader("FONT", FontInstaller::isValidFamilyName, FontInstaller::isValidFontFileName);
  const FontManifestStatus status = reader.read(manifestFile, families_, baseUrl_);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (status == FontManifestStatus::Invalid) {
    LOG_ERR("FONT", "Manifest parse error: %s", reader.failure());
    errorMessage_ = "Invalid font manifest";
    return false;
  }
  if (status == FontManifestStatus::UnsupportedVersion) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", reader.version());
    errorMessage_ = "Unsupported manifest version";
    return false;
  }
  if (status == FontManifestStatus::OutOfMemory) {
    LOG_ERR("FONT", "Manifest parse error: %s", reader.failure());
    errorMessage_ = "Failed to read font list";
    return false;
  }

  for (size_t i = 0; i < families_.count(); i++) {
    const char* familyName = families_.name(i);
    families_.setInstalled(i, fontInstaller_.isFamilyInstalled(familyName));

    if (families_.installed(i)) {
      for (size_t j = 0; j < families_.fileCount(i); j++) {
        char path[128];
        FontInstaller::buildFontPath(familyName, families_.fileLocalName(i, j), path, sizeof(path));
        FsFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != families_.fileSize(i, j)) {
            families_.setHasUpdate(i, true);
            break;
          }
        } else {
          families_.setHasUpdate(i, true);
          break;
        }
      }
    } else {
      // Surface leftover staging from a previously interrupted download so the
      // UI can offer "Resume" instead of restarting from scratch.
      char stagingDir[128];
      FontInstaller::buildStagingDirPath(familyName, stagingDir, sizeof(stagingDir));
      families_.setHasResumableDownload(i, Storage.exists(stagingDir));
    }
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu files, %zu bytes in one block", families_.count(),
          families_.totalFiles(), families_.blockBytes());
  return true;
}

// --- Stash/Restore ---
//
// Persist families_ to SD so its block can be freed for the download. The
// wolfSSL session needs a contiguous ~17 KB block for a TLS record. The list
// used to be ~300 scattered allocations, and freeing them left the heap in
// pieces too small for it (X3: largest block 10 KB, MEMORY_E, download
// failed); its one block now comes back whole. The file is the block itself,
// as FontCatalog documents it, written in one write and read back in one read.

static constexpr const char* FAMILIES_STASH_PATH = "/fonts_families.bin";

bool FontDownloadActivity::stashFamiliesToSd() {
  Storage.remove(FAMILIES_STASH_PATH);
  FsFile file;
  if (!Storage.openFileForWrite("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash open failed");
    return false;
  }

  const bool ok = families_.writeTo(file);
  file.flush();
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash write failed");
    Storage.remove(FAMILIES_STASH_PATH);
    return false;
  }
  // Free the in-memory copy now that it's safely on disk. Under the lock: the
  // render task reads the block's strings.
  const size_t bytes = families_.blockBytes();
  {
    RenderLock lock(*this);
    families_.clear();
  }
  LOG_DBG("FONT", "Stashed families_ (%zu bytes) to %s and cleared in-memory copy", bytes, FAMILIES_STASH_PATH);
  return true;
}

bool FontDownloadActivity::restoreFamiliesFromSd() {
  FsFile file;
  if (!Storage.openFileForRead("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash file missing");
    return false;
  }

  // Read into a block of its own, then hand it over under the lock, so the
  // render task never sees a half-read list.
  FontCatalog restored;
  const size_t fileBytes = file.fileSize();
  const bool ok = restored.readFrom(file);
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash read failed (%zu bytes)", fileBytes);
    return false;
  }
  {
    RenderLock lock(*this);
    families_.swap(restored);
  }
  // Keep the stash file around so a crash mid-download can still recover.
  // It gets overwritten on next stash and is harmless if stale.
  LOG_DBG("FONT", "Restored %zu families from stash", families_.count());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  // Snapshot indices upfront because downloadFamily() stashes/restores
  // families_ — indices remain valid as long as we don't sort or splice it.
  std::vector<int> targetIndices;
  targetIndices.reserve(families_.count());
  for (size_t i = 0; i < families_.count(); i++) {
    if (!families_.installed(i)) targetIndices.push_back(static_cast<int>(i));
  }
  for (int idx : targetIndices) {
    downloadFamily(idx);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  std::vector<int> targetIndices;
  targetIndices.reserve(families_.count());
  for (size_t i = 0; i < families_.count(); i++) {
    if (families_.installed(i) && families_.hasUpdate(i)) targetIndices.push_back(static_cast<int>(i));
  }
  for (int idx : targetIndices) {
    downloadFamily(idx);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

size_t FontDownloadActivity::totalUninstalledSize() const {
  size_t total = 0;
  for (size_t i = 0; i < families_.count(); i++) {
    if (!families_.installed(i)) total += families_.totalSize(i);
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (size_t i = 0; i < families_.count(); i++) {
    if (families_.installed(i) && families_.hasUpdate(i)) total += families_.totalSize(i);
  }
  return total;
}

void FontDownloadActivity::syncSelectedIndexForNewActionCount() {
  const int currentActionCount = actionCount();
  if (currentActionCount == previousActionCount_) {
    return;
  }

  int newIndex = selectedIndex_;
  if (selectedIndex_ >= previousActionCount_) {
    const int familyIndex = selectedIndex_ - previousActionCount_;
    newIndex = familyIndex + currentActionCount;
  } else if (selectedIndex_ >= currentActionCount) {
    newIndex = currentActionCount;
  }

  if (newIndex >= listItemCount()) {
    newIndex = std::max(0, listItemCount() - 1);
  }

  selectedIndex_ = newIndex;
  previousActionCount_ = currentActionCount;
}

bool FontDownloadActivity::hasDownloadCandidates() const {
  for (size_t i = 0; i < families_.count(); i++) {
    if (!families_.installed(i)) return true;
  }
  return false;
}

bool FontDownloadActivity::hasUpdateCandidates() const {
  for (size_t i = 0; i < families_.count(); i++) {
    if (families_.installed(i) && families_.hasUpdate(i)) return true;
  }
  return false;
}

void FontDownloadActivity::downloadFamily(int familyIdx) {
  if (familyIdx < 0 || familyIdx >= static_cast<int>(families_.count())) {
    LOG_ERR("FONT", "downloadFamily: invalid index %d (size %zu)", familyIdx, families_.count());
    return;
  }

  // Copy the target family into a block of its own (a few hundred bytes),
  // then stash + free families_ so the TLS session finds the heap the list
  // held in one piece. Render-path caches (downloadingFamilyName_,
  // downloadingFamilyHasResumable_) cover the family-name and Resume-label
  // accesses that previously read families_ during DOWNLOADING/ERROR.
  snprintf(downloadingFamilyName_, sizeof(downloadingFamilyName_), "%s", families_.name(familyIdx));
  downloadingFamilyHasResumable_ = families_.hasResumableDownload(familyIdx);
  FontCatalog family;
  if (!family.copyFamilyFrom(families_, familyIdx)) {
    LOG_ERR("FONT", "OOM: copy of family %s", downloadingFamilyName_);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to stash manifest";
    return;
  }

  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = familyIdx;
    currentFileIndex_ = 0;
    currentFileTotal_ = family.fileCount(0);
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  if (!stashFamiliesToSd()) {
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to stash manifest";
    return;
  }

  // Run the actual download with families_ empty (defragmented heap).
  downloadFamilyImpl(family, familyIdx);

  // Update cached render state from the impl's mutations.
  downloadingFamilyHasResumable_ = family.hasResumableDownload(0);

  // Restore families_ regardless of success/error/abort outcome, then merge
  // back the mutations the impl made on the local family copy. Without the
  // restored manifest the activity can't render the family list, so a failed
  // restore is fatal — drop to ERROR rather than continuing with empty state.
  if (!restoreFamiliesFromSd()) {
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to restore manifest";
    return;
  }
  if (familyIdx >= 0 && familyIdx < static_cast<int>(families_.count())) {
    families_.setInstalled(familyIdx, family.installed(0));
    families_.setHasUpdate(familyIdx, family.hasUpdate(0));
    families_.setHasResumableDownload(familyIdx, family.hasResumableDownload(0));
  }
  syncSelectedIndexForNewActionCount();
}

void FontDownloadActivity::downloadFamilyImpl(FontCatalog& family, int familyIdx) {
  // `family` holds this one family, at index 0.
  const char* familyName = family.name(0);

  // httpSession_ does the TLS handshake on its first downloadToFile call;
  // subsequent files reuse the open keep-alive connection. If the server
  // dropped the connection during the idle gap (user browsing the family
  // list), the Session layer transparently reinitialises and retries once.
  char liveDir[128];
  char stagingDir[128];
  char backupDir[128];
  snprintf(liveDir, sizeof(liveDir), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);
  FontInstaller::buildStagingDirPath(familyName, stagingDir, sizeof(stagingDir));
  FontInstaller::buildBackupDirPath(familyName, backupDir, sizeof(backupDir));

  // Resume-aware staging: if a __staging dir is left over from a previous
  // interrupted download, keep it so files already on disk can be reused.
  // Files are individually re-verified below (size + CRC) before being
  // accepted, so half-written files are caught.
  if (!Storage.exists(stagingDir) && !Storage.mkdir(stagingDir)) {
    LOG_ERR("FONT", "Failed to create staging dir: %s", stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to create staging area";
    return;
  }

  for (size_t i = 0; i < family.fileCount(0); i++) {
    const char* fileName = family.fileName(0, i);
    const uint32_t expectedSize = family.fileSize(0, i);
    const bool hasCrc32 = family.fileHasCrc32(0, i);
    const uint32_t expectedCrc32 = family.fileCrc32(0, i);

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = expectedSize;
      lastProgressPercent_ = -1;
      lastProgressUpdateMs_ = 0;
    }
    requestUpdateAndWait();

    char stagedPath[128];
    snprintf(stagedPath, sizeof(stagedPath), "%s/%s", stagingDir, family.fileLocalName(0, i));

    // If this file is already present in staging from a previous run and
    // matches the manifest, skip the download. CRC32 is checked when the
    // manifest carries one (v2+); otherwise size + magic-byte check is the
    // best we can do.
    if (Storage.exists(stagedPath)) {
      FsFile f;
      bool sizeOk = false;
      if (Storage.openFileForRead("FONT", stagedPath, f)) {
        sizeOk = (f.fileSize() == expectedSize);
        f.close();
      }
      bool crcOk = !hasCrc32;
      if (sizeOk && hasCrc32) {
        uint32_t actualCrc = 0;
        if (FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
          crcOk = (actualCrc == expectedCrc32);
        }
      }
      if (sizeOk && crcOk && fontInstaller_.validateCpfontFile(stagedPath)) {
        LOG_DBG("FONT", "Resuming: reusing %s", stagedPath);
        fileProgress_ = expectedSize;
        fileTotal_ = expectedSize;
        continue;
      }
      LOG_DBG("FONT", "Resuming: re-downloading stale %s (sizeOk=%d crcOk=%d)", stagedPath, sizeOk, crcOk);
      Storage.remove(stagedPath);
    }

    // Make sure parent directories exist for the file
    std::string stagedPathStr(stagedPath);
    size_t lastSlash = stagedPathStr.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(stagedPathStr.substr(0, lastSlash).c_str());
    }

    std::string url = baseUrl_ + fileName;

    auto result = HttpDownloader::downloadToFile(
        httpSession_, url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
          mappedInput.update();
          fileProgress_ = downloaded;
          fileTotal_ = total;

          const unsigned long now = millis();
          int percent = 0;
          if (total > 0) {
            percent = static_cast<int>((static_cast<unsigned long long>(downloaded) * 100ULL + total / 2) / total);
          }
          const bool percentChanged = percent != lastProgressPercent_;
          const bool timeElapsed = lastProgressUpdateMs_ == 0 || now - lastProgressUpdateMs_ > 2000;
          if ((percentChanged && timeElapsed) || downloaded == total) {
            requestUpdate(true);
            lastProgressPercent_ = percent;
            lastProgressUpdateMs_ = now;
          }

          return !mappedInput.wasPressed(MappedInputManager::Button::Back);
        });

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", fileName);
      // Keep staging dir so the next launch can resume.
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      cancelRequested_ = true;
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", fileName, result);
      // Drop just the file that failed; keep already-downloaded siblings so
      // the next retry resumes from here.
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = std::string("Download failed: ") + fileName;
      return;
    }

    // CRC32: matches upstream PR #1904 — catches truncated/torn writes.
    if (hasCrc32) {
      uint32_t actualCrc = 0;
      if (!FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
        LOG_ERR("FONT", "Failed to read for CRC: %s", stagedPath);
        Storage.remove(stagedPath);
        family.setHasResumableDownload(0, !family.installed(0));
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = std::string("Failed to verify: ") + fileName;
        return;
      }
      if (actualCrc != expectedCrc32) {
        LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", fileName, actualCrc, expectedCrc32);
        Storage.remove(stagedPath);
        family.setHasResumableDownload(0, !family.installed(0));
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = std::string("Checksum mismatch: ") + fileName;
        return;
      }
    }

    if (!fontInstaller_.validateCpfontFile(stagedPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", stagedPath);
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = std::string("Invalid font file: ") + fileName;
      return;
    }
  }

  const bool hadLiveDir = Storage.exists(liveDir);

  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    LOG_ERR("FONT", "Failed to clean backup dir: %s", backupDir);
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to prepare backup area";
    return;
  }

  if (hadLiveDir && !Storage.rename(liveDir, backupDir)) {
    LOG_ERR("FONT", "Failed to move live family to backup: %s", liveDir);
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to replace installed font";
    return;
  }

  if (!Storage.rename(stagingDir, liveDir)) {
    LOG_ERR("FONT", "Failed to activate staged family: %s", stagingDir);
    if (hadLiveDir && Storage.exists(backupDir)) {
      Storage.rename(backupDir, liveDir);
    }
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to finalize font install";
    return;
  }

  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    LOG_INF("FONT", "Failed to remove backup dir after successful install: %s", backupDir);
  }

  fontInstaller_.refreshRegistry();
  family.setInstalled(0, true);
  family.setHasUpdate(0, false);
  family.setHasResumableDownload(0, false);
  // syncSelectedIndexForNewActionCount() is deferred to downloadFamily() —
  // it needs families_ which is empty during this impl.

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.count())) return;
  const std::string heading = tr(STR_DELETE) + std::string("?");
  const std::string body = families_.name(familyIndex);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this, familyIndex](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           deleteFamilyAtIndex(familyIndex);
                         });
}

void FontDownloadActivity::deleteFamilyAtIndex(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.count())) return;

  const auto result = fontInstaller_.deleteFamily(families_.name(familyIndex));
  if (result == FontInstaller::Error::OK) {
    fontInstaller_.refreshRegistry();
    families_.setInstalled(familyIndex, false);
    families_.setHasUpdate(familyIndex, false);
    syncSelectedIndexForNewActionCount();
    pendingErrorAction_ = PendingFontAction::None;
    errorMessage_.clear();

    if (selectedIndex_ >= listItemCount()) {
      selectedIndex_ = std::max(0, listItemCount() - 1);
    }

    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    requestUpdate();
    return;
  }

  std::string message = "Failed to delete font";
  if (result == FontInstaller::Error::INVALID_FAMILY_NAME) {
    message = "Invalid font family";
  }

  RenderLock lock(*this);
  state_ = ERROR;
  downloadingFamilyIndex_ = familyIndex;
  pendingErrorAction_ = PendingFontAction::Delete;
  errorMessage_ = message;
}

std::string FontDownloadActivity::confirmButtonLabel() const {
  if (families_.empty()) return tr(STR_DOWNLOAD);
  if (isDownloadAllSelected()) return tr(STR_DOWNLOAD);
  if (isUpdateAllSelected()) return tr(STR_UPDATE);
  const int family = familyIndexFromList(selectedIndex_);
  if (families_.installed(family) && !families_.hasUpdate(family)) return tr(STR_DELETE);
  if (families_.hasUpdate(family)) return tr(STR_UPDATE);
  if (families_.hasResumableDownload(family)) return tr(STR_RESUME);
  return tr(STR_DOWNLOAD);
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    syncSelectedIndexForNewActionCount();
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextList(selectedIndex_, listItemCount(), [this] { requestUpdate(); });
    buttonNavigator_.onPreviousList(selectedIndex_, listItemCount(), [this] { requestUpdate(); });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        if (isDownloadAllSelected()) {
          downloadAll();
          requestUpdateAndWait();
        } else if (isUpdateAllSelected()) {
          updateAll();
          requestUpdateAndWait();
        } else {
          const int familyIndex = familyIndexFromList(selectedIndex_);
          if (families_.installed(familyIndex) && !families_.hasUpdate(familyIndex)) {
            promptDeleteFamily(familyIndex);
          } else {
            downloadFamily(familyIndex);
            requestUpdateAndWait();
          }
        }
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.count())) {
        if (pendingErrorAction_ == PendingFontAction::Delete) {
          deleteFamilyAtIndex(downloadingFamilyIndex_);
        } else {
          downloadFamily(downloadingFamilyIndex_);
        }
        requestUpdateAndWait();
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_MANAGER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    syncSelectedIndexForNewActionCount();
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) {
                return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalUninstalledSize()) + ")";
              }
              if (hasUpdateCandidates() && index == 1) {
                return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
              }
            } else if (hasUpdateCandidates() && index == 0) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_.name(familyIndexFromList(index));
          },
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) return "";
              if (hasUpdateCandidates() && index == 1) return "";
            } else if (hasUpdateCandidates() && index == 0) {
              return "";
            }
            return families_.description(familyIndexFromList(index));
          },
          nullptr,
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) return "";
              if (hasUpdateCandidates() && index == 1) return "";
            } else if (hasUpdateCandidates() && index == 0) {
              return "";
            }
            const int family = familyIndexFromList(index);
            if (families_.hasUpdate(family)) return tr(STR_UPDATE_AVAILABLE);
            if (families_.installed(family)) return tr(STR_INSTALLED);
            if (families_.hasResumableDownload(family)) return tr(STR_RESUME);
            return "";
          },
          true);

      const std::string confirmLabel = confirmButtonLabel();
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel.c_str(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    // families_ is stashed to SD during downloadFamily(); read the cached
    // name instead of indexing families_.
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + downloadingFamilyName_ + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, percentY,
                              (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    // Use the cached value: families_ may have just been restored (post-impl)
    // or still empty (if the failure was in the stash itself); either way the
    // cache reflects the last update from the download attempt.
    const bool canResume = pendingErrorAction_ == PendingFontAction::Download && downloadingFamilyHasResumable_;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), canResume ? tr(STR_RESUME) : tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

ListRowTap::Result FontDownloadActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, listItemCount(), selectedIndex_);
}
