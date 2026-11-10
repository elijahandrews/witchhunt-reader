#include "JpegToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <ProgressiveJpeg.h>

#include <cstddef>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
// The file I/O callbacks receive the FsFile* via pFile->fHandle (set by jpegOpen()).
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Source dimensions after JPEGDEC's built-in scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y axes use separate scale factors: the aspect ratio of the output (dstWidth/dstHeight)
  // may differ from the source (srcWidth/srcHeight) due to integer rounding of displayHeight.
  // Using a single (X-based) scale for both axes causes the wrong srcRow to be skipped
  // during nearest-neighbor downscaling, potentially losing critical image content.
  int32_t fineScaleFPX{1 << 16};  // X: src -> dst column mapping
  int32_t invScaleFPX{1 << 16};   // X: dst -> src column mapping
  int32_t fineScaleFPY{1 << 16};  // Y: src -> dst row mapping
  int32_t invScaleFPY{1 << 16};   // Y: dst -> src row mapping

  PixelCache cache;
  bool cancelled = false;
  bool emitted = false;
  bool caching{false};
  uint32_t lastYieldMs{0};
};

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
void* jpegOpen(const char* filename, int32_t* size) {
  auto f = makeUniqueNoThrow<FsFile>();
  if (!f) {
    LOG_ERR("JPG", "OOM: JPEG file handle (%u free, %u max alloc)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return nullptr;
  }
  if (!Storage.openFileForRead("JPG", std::string(filename), *f)) {
    return nullptr;
  }
  *size = f->size();
  return f.release();  // JPEGDEC owns this handle until jpegClose deletes it.
}

void jpegClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

// JPEGDEC tracks file position via pFile->iPos internally (e.g. JPEGGetMoreData
// checks iPos < iSize to decide whether more data is available). The callbacks
// MUST maintain iPos to match the actual file position, otherwise progressive
// JPEGs with large headers fail during parsing.
int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t bytesRead = f->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t jpegSeek(JPEGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  if (!f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// The parser uses this conservative admission budget before extracting an
// image. Keep it in sync with the actual pinned JPEGDEC object below.
static_assert(sizeof(JPEGDEC) <= MemoryBudget::JPEG_DECODER_APPROX_BYTES, "JPEGDEC exceeded its shared memory budget");
static_assert(alignof(JPEGDEC) <= alignof(std::max_align_t), "JPEGDEC needs unsupported heap alignment");

bool initJpegDecoder(HeapObject<JPEGDEC>& decoder, const char* imagePath) {
  const MemoryPool preferredPool = MemoryBudget::jpegDecoderPool(sizeof(JPEGDEC));
  if (preferredPool == MemoryPool::None) {
    MemoryBudget::hasHeapForJpegDecoder("JPG", sizeof(JPEGDEC), imagePath);
    return false;
  }

  if (decoder.init(preferredPool)) {
    const auto internal = byteHeapSnapshot(MemoryPool::Internal);
    const auto psram = byteHeapSnapshot(MemoryPool::Psram);
    LOG_DBG("JPG", "JPEG decoder pool=%s bytes=%u; internal free=%u max=%u; psram free=%u max=%u",
            memoryPoolName(decoder.pool()), static_cast<unsigned>(sizeof(JPEGDEC)),
            static_cast<unsigned>(internal.free), static_cast<unsigned>(internal.largest),
            static_cast<unsigned>(psram.free), static_cast<unsigned>(psram.largest));
    return true;
  }

  if (preferredPool != MemoryPool::Psram) {
    const auto internal = byteHeapSnapshot(MemoryPool::Internal);
    const auto psram = byteHeapSnapshot(MemoryPool::Psram);
    LOG_ERR("JPG",
            "Failed to allocate JPEG decoder in internal heap (%u bytes; internal free=%u max=%u; psram free=%u "
            "max=%u)",
            static_cast<unsigned>(sizeof(JPEGDEC)), static_cast<unsigned>(internal.free),
            static_cast<unsigned>(internal.largest), static_cast<unsigned>(psram.free),
            static_cast<unsigned>(psram.largest));
    return false;
  }

  // A pool snapshot is only a preflight. If PSRAM changed before allocation,
  // take a fresh snapshot and use the established internal route only when it
  // remains safe; never weaken the C3 threshold as a fallback.
  const auto internal = byteHeapSnapshot(MemoryPool::Internal);
  const auto psram = byteHeapSnapshot(MemoryPool::Psram);
  LOG_ERR("JPG",
          "Failed to allocate JPEG decoder in PSRAM (%u bytes; internal free=%u max=%u; psram free=%u max=%u); "
          "checking internal fallback",
          static_cast<unsigned>(sizeof(JPEGDEC)), static_cast<unsigned>(internal.free),
          static_cast<unsigned>(internal.largest), static_cast<unsigned>(psram.free),
          static_cast<unsigned>(psram.largest));
  if (!MemoryBudget::canUseInternalHeapForJpegDecoder(internal)) {
    MemoryBudget::hasHeapForJpegDecoder("JPG", sizeof(JPEGDEC), imagePath);
    return false;
  }
  if (!decoder.init(MemoryPool::Internal)) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder in safe internal fallback (%u bytes)",
            static_cast<unsigned>(sizeof(JPEGDEC)));
    return false;
  }

  LOG_DBG("JPG", "JPEG decoder pool=internal fallback bytes=%u; internal free=%u max=%u; psram free=%u max=%u",
          static_cast<unsigned>(sizeof(JPEGDEC)), static_cast<unsigned>(byteHeapSnapshot(MemoryPool::Internal).free),
          static_cast<unsigned>(byteHeapSnapshot(MemoryPool::Internal).largest),
          static_cast<unsigned>(byteHeapSnapshot(MemoryPool::Psram).free),
          static_cast<unsigned>(byteHeapSnapshot(MemoryPool::Psram).largest));
  return true;
}

// Choose JPEGDEC's built-in scale factor for coarse downscaling.
// Returns the scale denominator (1, 2, 4, or 8) and sets jpegScaleOption.
int chooseJpegScale(float targetScale, int& jpegScaleOption) {
  if (targetScale <= 0.125f) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    return 8;
  }
  if (targetScale <= 0.25f) {
    jpegScaleOption = JPEG_SCALE_QUARTER;
    return 4;
  }
  if (targetScale <= 0.5f) {
    jpegScaleOption = JPEG_SCALE_HALF;
    return 2;
  }
  jpegScaleOption = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;
  if (ctx->config->cancelled()) {
    ctx->cancelled = true;
    return 0;
  }

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (!ctx->config->cacheOnly && ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (!ctx->config->cacheOnly && dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (!ctx->config->cacheOnly && ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (!ctx->config->cacheOnly && dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw;
  pw.init(renderer, ctx->config->useGrayscale, ctx->config->cacheOnly);

  // The cache streams to disk one MCU-row band at a time. Flushing rows below
  // this block (raster order guarantees they are final) repositions the band;
  // cacheOriginY then maps screen rows to the band-local buffer rows. If a flush
  // write fails, stop caching for the rest of this decode (and let finalize drop
  // the partial file) rather than writing past the band buffer.
  DirectCacheWriter cw;
  int cacheOriginY = 0;
  if (caching) {
    if (!ctx->cache.advanceTo(dstYStart)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cacheOriginY = ctx->config->y + ctx->cache.bandStart;
    }
  }

  if (ctx->config->cacheOnly && !caching) return 0;
  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      if (ctx->config->cancelled()) {
        ctx->cancelled = true;
        return 0;
      }
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY, cacheOriginY);
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFPX > FP_ONE || fineScaleFPY > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      if (ctx->config->cancelled()) {
        ctx->cancelled = true;
        return 0;
      }
      const int outY = cfgY + dstY;
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY, cacheOriginY);
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Nearest-neighbor (downscale: fineScale < 1.0) ===
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    if (ctx->config->cancelled()) {
      ctx->cancelled = true;
      return 0;
    }
    const int outY = cfgY + dstY;
    pw.beginRow(outY);
    if (caching) cw.beginRow(outY, cacheOriginY);
    const int32_t srcFyFP = dstY * invScaleFPY;
    int ly = (srcFyFP >> FP_SHIFT) - blockY;
    if (ly < 0) ly = 0;
    if (ly >= blockH) ly = blockH - 1;
    const uint8_t* row = &pixels[ly * stride];

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFPX;
      int lx = (srcFxFP >> FP_SHIFT) - blockX;
      if (lx < 0) lx = 0;
      if (lx >= validW) lx = validW - 1;
      uint8_t gray = row[lx];

      uint8_t dithered;
      if (useDithering) {
        dithered = applyBayerDither4Level(gray, outX, outY);
      } else {
        dithered = gray / 85;
        if (dithered > 3) dithered = 3;
      }
      pw.writePixel(outX, dithered);
      if (caching) cw.writePixel(outX, dithered);
    }
  }

  return 1;
}

}  // namespace

namespace {
enum class ProgressiveAttempt { Complete, Failed, Fallback };

ProgressiveAttempt decodeFullProgressive(FsFile& source, const ProgressiveJpeg::ImageInfo& info,
                                         GfxRenderer& renderer, const RenderConfig& config) {
  auto context = makeUniqueNoThrow<JpegContext>();
  if (!context) return ProgressiveAttempt::Fallback;
  auto& ctx = *context;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  float targetScale;
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    targetScale = static_cast<float>(ctx.dstWidth) / info.width;
  } else {
    targetScale = std::min(1.0f, std::min(config.maxWidth > 0 ? static_cast<float>(config.maxWidth) / info.width : 1.0f,
                                        config.maxHeight > 0 ? static_cast<float>(config.maxHeight) / info.height : 1.0f));
    ctx.dstWidth = static_cast<int>(info.width * targetScale);
    ctx.dstHeight = static_cast<int>(info.height * targetScale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) return ProgressiveAttempt::Failed;
  unsigned shift = 0;
  while (shift < 3 && targetScale <= 1.0f / (2u << shift) && (info.width >> (shift + 1)) && (info.height >> (shift + 1))) ++shift;

  // One bounded coefficient band, never a whole-image coefficient/frame buffer.
  // Prefer PSRAM; internal fallback keeps room for native cache bands and input.
  constexpr size_t maximumWorkspace = 384U * 1024U;
  constexpr size_t internalReserve = 32U * 1024U;
  HeapByteBuffer workspace;
  size_t workspaceSize = 0;
  for (; shift <= 3; ++shift) {
    if (!(info.width >> shift) || !(info.height >> shift)) break;
    workspaceSize = ProgressiveJpeg::workspaceBytes(info, static_cast<uint8_t>(shift));
    if (!workspaceSize || workspaceSize > maximumWorkspace) continue;
    const auto psram = byteHeapSnapshot(MemoryPool::Psram);
    if (psram.free >= workspaceSize + MemoryBudget::EPUB_PSRAM_RESERVE && psram.largest >= workspaceSize)
      workspace = makePsramByteBufferNoThrow(workspaceSize);
    if (!workspace) {
      const auto internal = byteHeapSnapshot(MemoryPool::Internal);
      if (internal.free >= workspaceSize + internalReserve && internal.largest >= workspaceSize)
        workspace = makeInternalByteBufferNoThrow(workspaceSize);
    }
    if (workspace) break;
  }
  if (!workspace) return ProgressiveAttempt::Fallback;
  ctx.scaledSrcWidth = info.width >> shift;
  ctx.scaledSrcHeight = info.height >> shift;
  ctx.fineScaleFPX = static_cast<int32_t>((int64_t(ctx.dstWidth) * FP_ONE) / ctx.scaledSrcWidth);
  ctx.fineScaleFPY = static_cast<int32_t>((int64_t(ctx.dstHeight) * FP_ONE) / ctx.scaledSrcHeight);
  ctx.invScaleFPX = static_cast<int32_t>((int64_t(ctx.scaledSrcWidth) * FP_ONE) / ctx.dstWidth);
  ctx.invScaleFPY = static_cast<int32_t>((int64_t(ctx.scaledSrcHeight) * FP_ONE) / ctx.dstHeight);
  ctx.lastYieldMs = millis();
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  options.workspace = workspace.get();
  options.workspaceSize = workspaceSize;
  options.abortUser = &ctx;
  options.shouldAbort = [](void* user) {
    auto& state = *static_cast<JpegContext*>(user);
    ImageToFramebufferDecoder::yieldDuringDecode(state.lastYieldMs);
    if (state.config->cancelled()) state.cancelled = true;
    return state.cancelled;
  };
  ProgressiveJpeg::Decoder decoder;
  auto result = decoder.begin(source, options,
      [](void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows, uint16_t stride) {
        auto& state = *static_cast<JpegContext*>(user);
        state.emitted = true;
        JPEGDRAW draw{};
        draw.pUser = user;
        draw.y = y;
        draw.iWidth = stride;
        draw.iWidthUsed = width;
        draw.iHeight = rows;
        draw.pPixels = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(gray));
        return jpegDrawCallback(&draw) != 0;
      }, &ctx);
  if (result != ProgressiveJpeg::Result::Ok) {
    return result == ProgressiveJpeg::Result::Unsupported || result == ProgressiveJpeg::Result::OutOfMemory
               ? ProgressiveAttempt::Fallback : ProgressiveAttempt::Failed;
  }
  if (!config.cachePath.empty()) {
    const int bandRows = static_cast<int>((int64_t(16) * ctx.fineScaleFPY) >> FP_SHIFT) + 2;
    ctx.caching = ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, bandRows);
  }
  if (config.cacheOnly && !ctx.caching) return ProgressiveAttempt::Failed;
  do {
    result = decoder.step(1);
    // Bounded index/band steps yield even when a small file has no abort checkpoint.
    ImageToFramebufferDecoder::yieldDuringDecode(ctx.lastYieldMs);
    if (config.cancelled()) ctx.cancelled = true;
  } while (result == ProgressiveJpeg::Result::Pending && !ctx.cancelled);
  decoder.end();
  if (ctx.cancelled) return ProgressiveAttempt::Failed;
  if (result != ProgressiveJpeg::Result::Ok) {
    return !ctx.emitted && (result == ProgressiveJpeg::Result::Unsupported || result == ProgressiveJpeg::Result::OutOfMemory)
               ? ProgressiveAttempt::Fallback : ProgressiveAttempt::Failed;
  }
  if (!source.close() || config.cancelled()) return ProgressiveAttempt::Failed;
  if (ctx.caching) {
    const bool complete = ctx.cache.finalize();
    if (config.cacheOnly && !complete) return ProgressiveAttempt::Failed;
  } else if (config.cacheOnly) return ProgressiveAttempt::Failed;
  return ProgressiveAttempt::Complete;
}
}  // namespace

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  HeapObject<JPEGDEC> jpeg;
  if (!initJpegDecoder(jpeg, imagePath.c_str())) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder for dimensions");
    return false;
  }

  // JPEGDEC does not close a file when JPEGInit rejects a malformed header,
  // and close() is not idempotent. Keep exactly one close after every attempt.
  const auto closeJpeg = ScopedCleanup{[&jpeg] { jpeg->close(); }};
  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, nullptr);
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG for dimensions (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  out.width = jpeg->getWidth();
  out.height = jpeg->getHeight();

  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  if (config.cancelled() || (config.cacheOnly && config.cachePath.empty())) return false;
  if (!config.sourceData) {
    FsFile source;
    if (!Storage.openFileForRead("JPG", imagePath, source)) return false;
    const auto closeSource = ScopedCleanup{[&source] { source.close(); }};
    ProgressiveJpeg::ImageInfo info;
    if (ProgressiveJpeg::probe(source, info) == ProgressiveJpeg::Result::Ok) {
      if (!validateImageDimensions(info.width, info.height, "JPEG", MAX_JPEG_SOURCE_WIDTH)) return false;
      const auto result = decodeFullProgressive(source, info, renderer, config);
      if (result != ProgressiveAttempt::Fallback) return result == ProgressiveAttempt::Complete;
      LOG_DBG("JPG", "Full progressive decoder unavailable; trying native DC preview");
    }
  }
  HeapObject<JPEGDEC> jpeg;
  if (!initJpegDecoder(jpeg, imagePath.c_str())) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  // Keep exactly one close after every attempted open, including malformed
  // JPEGs where JPEGDEC returns from JPEGInit without closing its file handle.
  const auto closeJpeg = ScopedCleanup{[&jpeg] { jpeg->close(); }};
  int rc = config.sourceData ? jpeg->openRAM(const_cast<uint8_t*>(config.sourceData),
                                             static_cast<int>(config.sourceSize), jpegDrawCallback)
                             : jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDrawCallback);
  if (rc != 1 || ctx.cancelled || config.cancelled()) {
    LOG_ERR("JPG", "Failed to open JPEG (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  int srcWidth = jpeg->getWidth();
  int srcHeight = jpeg->getHeight();

  if (srcWidth <= 0 || srcHeight <= 0) {
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    return false;
  }

  if (!validateImageDimensions(srcWidth, srcHeight, "JPEG", MAX_JPEG_SOURCE_WIDTH)) {
    return false;
  }

  bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (isProgressive) {
    LOG_INF("JPG", "Progressive JPEG detected - decoding DC coefficients only (lower quality)");
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Choose JPEGDEC built-in scaling for coarse downscaling.
  // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only
  // decode produces 1/8 resolution). We must match this to avoid the if/else
  // priority chain in DecodeJPEG selecting a different scale.
  int jpegScaleOption;
  int jpegScaleDenom;
  if (isProgressive) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    jpegScaleDenom = 8;
  } else {
    jpegScaleDenom = chooseJpegScale(targetScale, jpegScaleOption);
  }

  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Degenerate output dimensions %dx%d for %s, skipping render", destWidth, destHeight,
            imagePath.c_str());
    return false;
  }

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  if (ctx.scaledSrcWidth <= 0 || ctx.scaledSrcHeight <= 0) {
    LOG_ERR("JPG", "Invalid scaled JPEG dimensions: src=%dx%d scaled=%dx%d dst=%dx%d", srcWidth, srcHeight,
            ctx.scaledSrcWidth, ctx.scaledSrcHeight, destWidth, destHeight);
    return false;
  }

  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  // Set pixel type to 8-bit grayscale (must be after open())
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  // Start streaming the pixel cache to disk. The band only needs to hold the
  // tallest single decode block: a JPEGDEC MCU cell is at most 16 scaled-source
  // rows tall, which our fine scale maps to this many output rows.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    const int maxBlockDstRows = (int)(((int64_t)16 * ctx.fineScaleFPY) >> FP_SHIFT) + 2;
    if (!ctx.cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxBlockDstRows)) {
      LOG_ERR("JPG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  ctx.lastYieldMs = millis();
  if (config.cacheOnly && !ctx.caching) return false;
  const uint32_t decodeStarted = millis();
  rc = jpeg->decode(0, 0, jpegScaleOption);
  LOG_DBG("JPG", "Decoded %s: ok=%d time=%ums", imagePath.c_str(), rc == 1,
          static_cast<unsigned>(millis() - decodeStarted));

  if (rc != 1 || ctx.cancelled || config.cancelled()) {
    LOG_ERR("JPG", "Decode failed (rc=%d, lastError=%d)", rc, jpeg->getLastError());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  // Finalize the streamed cache file. Note: a flush failure mid-decode clears
  // ctx.caching (the partial file is dropped), so re-read the flag here.
  if (ctx.caching) {
    const bool complete = ctx.cache.finalize();
    if (config.cacheOnly) return complete;
  }

  return !config.cacheOnly;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
