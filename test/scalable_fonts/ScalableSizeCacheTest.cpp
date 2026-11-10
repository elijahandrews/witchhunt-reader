#include <ScalableSizeCache.h>

#include <cassert>
#include <map>
struct Renderer {
  std::map<int, EpdFontFamily> fonts;
  unsigned failures = 0;
  const auto& getFontMap() const { return fonts; }
  void insertFont(int id, EpdFontFamily family) { fonts.emplace(id, family); }
  void removeFont(int id) { fonts.erase(id); }
  void noteFontFailure() { ++failures; }
};
int main() {
  Renderer renderer;
  HalScalableFont storage[4];
  HalScalableFont* faces[]{storage, storage + 1, storage + 2, storage + 3};
  ScalableSizeCache cache;
  assert(cache.resolve(renderer, faces, 700, 1700) == 1700);
  const EpdFont* borrowed = renderer.fonts.at(1700).faces[0];
  storage[2].fail = true;
  assert(!cache.resolve(renderer, faces, 717, 1717));
  for (auto& face : storage) assert(face.descriptors.size() == 1);
  assert(borrowed->points == 700);
  assert(renderer.fonts.size() == 1);
  assert(renderer.failures == 1);
  const auto calls = storage[0].calls;
  storage[2].fail = false;
  assert(!cache.resolve(renderer, faces, 717, 1717));
  assert(storage[0].calls == calls);
  cache.retryFailures();
  assert(cache.resolve(renderer, faces, 717, 1717) == 1717);
  int previous = 1717;
  for (unsigned points = 718; points < 918; ++points) {
    const int id = 1000 + points;
    assert(cache.resolve(renderer, faces, points, id) == id);
    assert(renderer.fonts.count(previous));
    assert(renderer.fonts.size() <= 32);
    for (auto& face : storage) assert(face.descriptors.size() == renderer.fonts.size());
    previous = id;
  }
  cache.clear(renderer, faces);
  assert(renderer.fonts.empty());
  for (auto& face : storage) assert(face.descriptors.empty());
  // A registry collision never overwrites or releases somebody else's font.
  EpdFont external;
  renderer.fonts.emplace(77, EpdFontFamily(&external, nullptr, nullptr, nullptr));
  assert(!cache.resolve(renderer, faces, 717, 77));
  assert(renderer.fonts.at(77).faces[0] == &external);
  for (auto& face : storage) assert(face.descriptors.empty());
}
