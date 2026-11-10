#pragma once
namespace library {
inline unsigned invalidations = 0;
inline void invalidateLibraryIndex() { ++invalidations; }
}  // namespace library
