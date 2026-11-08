# Direct font resize feedback

`run.py` extracts the production EPUB override methods and direct font-size
switch cases into a host probe. It uses the real reader size ladder and replaces
hardware, persistence and font I/O with ordered assertions.

Checks cover feedback before saving/loading, exclusive rendering ownership,
clearing stale previews and deferred AA, unchanged background-preemption budget,
minimum/maximum size no-ops, and the absence of duplicate menu feedback. Guards
cover disabled feedback, pending clean refresh, measured slow panels, unavailable
framebuffers and a borrowed buffer containing the next page. `--sanitize` enables
ASan/UBSan. These checks do not measure physical panel appearance or timing.
