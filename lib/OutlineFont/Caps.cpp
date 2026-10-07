#include "OpenTypeCaps.h"
namespace {
struct V {
  const uint8_t* p = nullptr;
  size_t n = 0;
  bool has(size_t o, size_t c) const { return o <= n && c <= n - o; }
  unsigned u16(size_t o) const { return has(o, 2) ? unsigned(p[o]) * 256 + p[o + 1] : 0; }
  uint32_t u32(size_t o) const { return uint32_t(u16(o)) << 16 | u16(o + 2); }
  V sub(size_t o) const { return o && has(o, 1) ? V{p + o, n - o} : V{}; }
};
int coverage(V v, unsigned gid) {
  unsigned n = v.u16(2);
  if (n > 8192) return -1;
  if (v.u16(0) == 1 && v.has(4, size_t(n) * 2)) {
    unsigned l = 0, h = n;
    while (l < h) {
      unsigned m = (l + h) / 2;
      if (v.u16(4 + m * 2) < gid)
        l = m + 1;
      else
        h = m;
    }
    return l < n && v.u16(4 + l * 2) == gid ? int(l) : -1;
  }
  if (v.u16(0) == 2 && v.has(4, size_t(n) * 6)) {
    for (unsigned i = 0; i < n; ++i) {
      size_t o = 4 + i * 6;
      if (gid >= v.u16(o) && gid <= v.u16(o + 2)) return int(v.u16(o + 4) + gid - v.u16(o));
    }
  }
  return -1;
}
V language(V scripts, uint32_t tag) {
  unsigned n = scripts.u16(0);
  if (!scripts.has(2, size_t(n) * 6)) return {};
  unsigned l = 0, h = n;
  while (l < h) {
    unsigned m = (l + h) / 2;
    if (scripts.u32(2 + m * 6) < tag)
      l = m + 1;
    else
      h = m;
  }
  if (l >= n || scripts.u32(2 + l * 6) != tag) return {};
  V s = scripts.sub(scripts.u16(6 + l * 6));
  V ls = s.sub(s.u16(0));
  return ls.has(0, 6) && ls.has(6, size_t(ls.u16(4)) * 2) ? ls : V{};
}
}  // namespace
uint16_t outlineCapsSubstitution(const uint8_t* p, size_t n, uint16_t gid, uint32_t tag) {
  V g{p, n};
  if (!p || !gid || !g.has(0, 10) || g.u16(0) != 1) return 0;
  V fs = g.sub(g.u16(6)), ls = g.sub(g.u16(8)), lang = language(g.sub(g.u16(4)), 0x6c61746e);
  if (!lang.p) lang = language(g.sub(g.u16(4)), 0x44464c54);
  if (!lang.p || !fs.has(2, size_t(fs.u16(0)) * 6) || !ls.has(2, size_t(ls.u16(0)) * 2)) return 0;
  unsigned work = 0;
  uint16_t current = gid;
  bool mapped = false;
  auto feature = [&](unsigned fi) {
    if (fi >= fs.u16(0) || ++work > 4096) return false;
    size_t r = 2 + fi * 6;
    if (fs.u32(r) != tag) return true;
    V f = fs.sub(fs.u16(r + 4));
    unsigned count = f.u16(2);
    if (!f.has(4, size_t(count) * 2)) return false;
    for (unsigned i = 0; i < count; ++i) {
      if (++work > 4096) return false;
      unsigned li = f.u16(4 + i * 2);
      if (li >= ls.u16(0)) return false;
      V l = ls.sub(ls.u16(2 + li * 2));
      unsigned type = l.u16(0), sc = l.u16(4);
      if (!l.has(0, 6) || !l.has(6, size_t(sc) * 2) || (type != 1 && type != 7) || l.u16(2) != 0) return false;
      for (unsigned j = 0; j < sc; ++j) {
        if (++work > 4096) return false;
        V s = l.sub(l.u16(6 + j * 2));
        if (type == 7) {
          if (!s.has(0, 8) || s.u16(0) != 1 || s.u16(2) != 1) return false;
          s = s.sub(s.u32(4));
        }
        if (!s.has(0, 6)) return false;
        int ci = coverage(s.sub(s.u16(2)), current);
        if (ci < 0) continue;
        uint16_t result = 0;
        if (s.u16(0) == 1)
          result = uint16_t(current + s.u16(4));
        else if (s.u16(0) == 2) {
          unsigned count = s.u16(4);
          if (unsigned(ci) >= count || !s.has(6, size_t(count) * 2)) return false;
          result = uint16_t(s.u16(6 + ci * 2));
        } else
          return false;
        if (!result) return false;
        current = result;
        mapped = true;
        break;
      }
    }
    return true;
  };
  unsigned req = lang.u16(2);
  if (req != 0xffff && !feature(req)) return 0;
  for (unsigned i = 0; i < lang.u16(4); ++i)
    if (!feature(lang.u16(6 + i * 2))) return 0;
  return mapped ? current : 0;
}
