# Capture-gap reporting evidence — 2026-09-28

Generated locally during the isolated host-analysis stage. No serial command,
upload, reset, storage clear or hardware experiment was performed. Main remained
at `43c3baf`; the analysis, tests and earlier storage extraction are uncommitted.
The full gate ran against that working tree, not a clean committed release.

## Files and provenance

- [capture-gap-report.txt](capture-gap-report.txt): stdout of the new
  `tools/charger-transitions.py` against a frozen copy of `data/samples.csv`,
  using defaults: max adjacent capture spacing 2 seconds inclusive and five
  consecutive classified samples. Host offsets and file order are preserved.
- [samples.csv.gz](samples.csv.gz): lossless gzip of the exact analysis input;
  decompression was checked byte-for-byte. The live CSV was read only.
- [input-summary.json](input-summary.json): independent CSV row count, time
  endpoints, experiment counts, timestamp-spacing median and raw SHA-256.
- [software-gate.txt](software-gate.txt): complete final `tools/check.sh` output,
  exit 0, ALL OK; **486 passed, 1 xfailed**, all seven gate steps passing.
- [initial-gate-path-failure.txt](initial-gate-path-failure.txt): first attempted
  gate; Python steps exited 127 because uv/uvx were missing from this session's
  PATH. Firmware compile, IntelliSense, shell and whitespace passed. Rerun used
  the already installed `/Users/afxjzs/.local/bin`; no dependencies changed.
- [SHA256SUMS](SHA256SUMS): checksums of the five evidence files above.

A targeted type check between the two gates found two typing errors in the new
script/test helper; both were fixed before the final gate. See LAB_NOTES.
The gate ran before documentation updates; doc links, whitespace and evidence
checksums were checked separately afterwards. The gate's untracked-file notice
means its Git whitespace check did not cover those files; the new source/test
and documentation files received a separate whitespace check.

## Input and implementation identity

Uncompressed input: **26,674 rows**, 2,057,536 bytes,
all Experiment 3. First timestamp `2026-09-16T10:07:44.231779-07:00`; last `2026-09-27T22:21:22.306909-07:00`.
Positive adjacent timestamp delta median: 1.000138 s.

```text
raw samples.csv SHA-256: af01adf4fb2d6dbd053abc5d3943f890dd0e6eb907dc898053a6c838b98a9714
charger-transitions.py SHA-256: a9ee811b2d8c51c160262041c998ce5488353ee49a57537e2247458c8fb4912a
test_charger_transitions.py SHA-256: c2c22784e807dd8b6c89460914e1561181f7954941683be971fcc579f439fc91
```

The archived input and code fingerprints identify this run even if the live CSV
later grows. The earlier temporary draft is not the delivered implementation.

## Results and limits

**81 capture segments, 80 gaps, 2 contiguous transitions, 39 other state
observations** (one initial and 38 post-gap). The two edge brackets remain
September 24 14:09:49.468777–14:09:50.468071 and
14:48:16.097899–14:48:17.099301, UTC-07:00. Confirmation occurs four later samples
after each first supporting sample. Exact physical switching instants are unknown.

The archived CSV includes 77 September 25 morning rows and 165 September 27
evening rows following long gaps; these do not establish continuous overnight
or weekend capture. Host receipt timestamps may reflect buffering/jitter. The
numeric settings are provisional analysis choices, not production firmware
thresholds, solar-availability detection or cause inference. There is still no
matching OFF-edge LED observation. A software gate does not validate the
unuploaded storage module on hardware or establish current board identity.
