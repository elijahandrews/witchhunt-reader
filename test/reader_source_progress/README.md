# Reader source progress checks

```sh
python3 test/reader_source_progress/run.py --out /tmp/reader-source-progress --sanitize
```

Compiles the production `sourceForProgress()` implementation and progress-load
and close-save statements extracted verbatim, together with the actual `EpubProgressRecord`
codec. The activity and Section shells replace device dependencies; assertions
cover pending targets during indexing, sticky split-word character offsets,
repeated reflows without navigation, deliberate navigation, live-build ready-page saves without an estimated total, legacy paragraph
fallback, and source restoration independent of page count. This does not test
physical storage, the full reader activity, or display rendering.

The record unit tests cover literal bytes and torn/unknown tails. The pipeline
`SourceAnchorTest.Persisted*` cases cover real EPUB/Section layout and persistence
across long/nested paragraph and publisher-margin reflows using host metrics.
