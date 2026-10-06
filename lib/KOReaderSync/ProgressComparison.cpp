#include "ProgressComparison.h"

#include <cmath>

namespace {
// Rounding noise between two percentages of the same book, not a real difference.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

ProgressComparison byPercentage(const float local, const float remote) {
  if (!std::isfinite(local) || !std::isfinite(remote)) {
    return ProgressComparison::Unknown;
  }
  const float delta = local - remote;
  if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) return ProgressComparison::Synchronized;
  return delta > 0.0f ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
}
}  // namespace

ProgressComparison compareProgress(const LocalReadingPosition& local, const float localPercentage,
                                   const CrossPointPosition& remote, const float remotePercentage) {
  if (remote.hasResolvedSpineIndex) {
    if (local.spineIndex != remote.spineIndex) {
      return local.spineIndex > remote.spineIndex ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
    }
    // Same chapter. The paragraph LUT answers exactly where a remote p[K] would open: on the local
    // page when K(p-1) < K <= K(p), before it when K <= K(p-1), after it otherwise.
    if (remote.hasParagraphIndex && local.paragraphAtPageEnd > 0) {
      const uint16_t k = remote.paragraphIndex;
      if (k <= local.paragraphAtPreviousPageEnd) return ProgressComparison::LocalAhead;
      if (k <= local.paragraphAtPageEnd) return ProgressComparison::Synchronized;
      return ProgressComparison::RemoteAhead;
    }
  }
  return byPercentage(localPercentage, remotePercentage);
}

RemoteRecordChoice selectRemoteRecord(const CrossPointPosition& primary, const float primaryPercentage,
                                      const CrossPointPosition& alternate, const float alternatePercentage) {
  if (primary.hasResolvedSpineIndex && alternate.hasResolvedSpineIndex) {
    if (primary.spineIndex != alternate.spineIndex) {
      return alternate.spineIndex > primary.spineIndex ? RemoteRecordChoice::Alternate : RemoteRecordChoice::Primary;
    }
    if (primary.hasParagraphIndex && alternate.hasParagraphIndex) {
      return alternate.paragraphIndex > primary.paragraphIndex ? RemoteRecordChoice::Alternate
                                                               : RemoteRecordChoice::Primary;
    }
  }
  return byPercentage(primaryPercentage, alternatePercentage) == ProgressComparison::RemoteAhead
             ? RemoteRecordChoice::Alternate
             : RemoteRecordChoice::Primary;
}
