# Reader source progress checks

```sh
python3 test/reader_source_progress/run.py --out /tmp/reader-source-progress --sanitize
```

Compiles the production `sourceForProgress()` implementation and progress-load
and close-save statements extracted verbatim, together with the actual `EpubProgressRecord`
codec and the real NavigationTarget declaration/resolver methods. The activity and Section shells replace device dependencies; assertions
cover pending targets during indexing, sticky split-word character offsets,
repeated reflows without navigation, deliberate navigation, live-build ready-page saves without an estimated total, legacy paragraph
fallback, source restoration independent of page count, and distinct raw-source versus KOReader visible-text coordinate resolution during live builds. This does not test
physical storage, the full reader activity, or display rendering.

The record unit tests cover literal bytes and torn/unknown tails. The pipeline
`SourceAnchorTest.Persisted*` cases cover real EPUB/Section layout and persistence
across long/nested paragraph and publisher-margin reflows using host metrics.

The same probe extracts real page-turn, jump and render-classification functions.
It checks a prepared-page hit followed by another queued forward/backward action,
a multi-page jump, or a chapter crossing before rendering, plus refused navigation
and stale handoff identity. `--negative-control keep-handoff` restores the old
invalidation bug; `--negative-control skip-identity` removes the defensive page
check. Both mutations must fail. No physical panel or scheduling latency is simulated.
