"""Apply the bounded/alignment-safe PNGdec 1.1.6 LZ copy fix.

The pinned decoder's fast path performs unaligned uint32_t loads/stores and
can write three bytes beyond the requested output span. Preserve its overlap
semantics with memcpy chunks and a strictly bounded byte tail. License remains
in the dependency; only the exact reviewed block is replaced.
"""
from pathlib import Path
import sys

try:
    Import("env")  # SCons injection
except NameError:
    env = None

BEFORE = r'''#ifdef ALLOWS_UNALIGNED
            {
                uint8_t *pEnd = put + copy;
                int overlap = (int)(intptr_t)(put - from);
                if (overlap >= 4)
                { // overlap of source/dest won't impede normal copy
                    while (put < pEnd - 3)
                    { // overwriting the output buffer here would be bad, so respect the true length
                        *(uint32_t *)put = *(uint32_t *)from;
                        put += 4;
                        from += 4;
                    }
                    while (put < pEnd)
                    { // tail end
                        *put++ = *from++;
                    }
                }
                else if (overlap == 1)
                { // copy 1-byte pattern
                    uint32_t pattern = *from;
                    pattern = pattern | (pattern << 8);
                    pattern = pattern | (pattern << 16);
                    while (put < pEnd)
                    {
                        *(uint32_t *)put = pattern;
                        put += 4;
                    }
                    put = pEnd; // correct possible overshoot
                }
                else
                { // overlap of 2 or 3
                    while (put < pEnd)
                    {
                        *put++ = *from++;
                    }
                }
            }
'''

AFTER = r'''#ifdef ALLOWS_UNALIGNED
            { /* CrossInk: bounded, alignment-safe LZ copies (PNGdec 1.1.6). */
                unsigned bytesLeft = copy;
                /* Integer addresses avoid subtraction between the separate
                   history window and output allocations. */
                uintptr_t distance = (uintptr_t)put - (uintptr_t)from;
                if (distance >= 4)
                {
                    while (bytesLeft >= 4)
                    {
                        memcpy(put, from, 4);
                        put += 4;
                        from += 4;
                        bytesLeft -= 4;
                    }
                    while (bytesLeft--)
                        *put++ = *from++;
                }
                else if (distance == 1)
                {
                    uint32_t pattern = *from;
                    pattern |= pattern << 8;
                    pattern |= pattern << 16;
                    while (bytesLeft >= 4)
                    {
                        memcpy(put, &pattern, 4);
                        put += 4;
                        bytesLeft -= 4;
                    }
                    while (bytesLeft--)
                        *put++ = (unsigned char)pattern;
                }
                else
                {
                    while (bytesLeft--)
                        *put++ = *from++;
                }
            }
'''

def apply(root):
    root = Path(root)
    version = (root / "library.properties").read_text()
    if "version=1.1.6" not in version.splitlines():
        raise RuntimeError("PNGdec patch requires reviewed version 1.1.6")
    source = root / "src/inflate.c"
    text = source.read_text()
    if text.count(AFTER) == 1 and BEFORE not in text:
        return False
    if text.count(BEFORE) != 1 or AFTER in text:
        raise RuntimeError("PNGdec inflate.c diverged; refusing an unverified patch")
    source.write_text(text.replace(BEFORE, AFTER, 1))
    return True

if env is not None:
    deps = Path(env["PROJECT_DIR"]) / ".pio/libdeps"
    for library in sorted(deps.glob("*/PNGdec")):
        apply(library)
elif __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_pngdec.py <PNGdec source directory>")
    apply(sys.argv[1])
