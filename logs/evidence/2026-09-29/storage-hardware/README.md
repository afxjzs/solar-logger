# Storage 1980d94 — hardware acceptance, 2026-09-29

**Healthy-log smoke passed.** Board reports `0.3.0-dev / 1980d94 /
solar-logger-protocol-ack-v3`. Experiment 3 retained; final dump 10,664 records,
sequences 1412–12075, board invalid 0. All 10,659 preflight decoded records are
identical in the final dump; new boot 36 adds 3 records, 12073–12075.
FIRST_AFTER_BOOT is set on 12073 and clear on subsequent records.

## Evidence and provenance

Direct execution on the local Mac using only canonical tools/send.sh and
tools/upload.sh for hardware. Every command has an unchanged `.txt` combined
stdout/stderr capture and adjacent `.json` with exact argv, host times, return
status, byte count and SHA-256. The wrapper captures bytes emitted by the tool;
it does not alter the tool or open the serial port itself. Sender output is
normalized decoded text, not raw serial bytes or a binary filesystem backup.
No complete cold-boot transcript is claimed; the sender discards pre-ACK chatter.

- [preflight.md](preflight.md): sealed read-only pre-upload assessment.
- [pre-dump.txt](pre-dump.txt), [pre-dump-validation.json](pre-dump-validation.json):
  10,659 records, 1412–12070; all 5,756 September 25 record lines unchanged.
- [upload.txt](upload.txt): one successful canonical upload, injected clean
  1980d94. Flash 1,104,265 B, globals 36,332 B. Programmed regions exclude the
  NVS and LittleFS partitions; normal targeted erase/reset is visible in the log.
- [post-version.txt](post-version.txt): actual board identity after upload.
- [post-storage-info.txt](post-storage-info.txt),
  [post-growth-info.txt](post-growth-info.txt): 10,661/12072 then 10,663/12074;
  72-byte records, trailing 0, INTACT, no storage errors.
- [post-hold.txt](post-hold.txt), [post-keepalive.txt](post-keepalive.txt),
  [post-held-status.txt](post-held-status.txt),
  [post-session-status.txt](post-session-status.txt),
  [post-release.txt](post-release.txt): matching ACK/RESULT OK, restored exp=3,
  interval 479, then RELEASE closes 3.788 s, saves 480 and sleeps armed.
- [post-dump.txt](post-dump.txt), [post-dump-validation.json](post-dump-validation.json):
  complete 10,664-record dump and exact comparison against every preflight record.
- startup-git.txt / startup-hashes.json: Git and source/document baseline.
- PREFLIGHT_SHA256SUMS seals the immutable pre-upload set; SHA256SUMS covers the
  completed set. capture-command.py, validate-dump.py and post-commands.py preserve
  the host capture/validation procedure; they are evidence helpers, not changes
  to production tooling. Temporary paths inside them record this run's context.

Both dumps contain complete indices, BEGIN/END, count summary, ACK and RESULT OK.
Their capture stops after the result when normal autonomous sleep removes the
port. The sender's generic “expected for RELEASE” text does not mean RELEASE was
issued for either dump. Neither dump reached its 120-second cap. No storage error
or damaged record is present; all epochs remain 0/time UNKNOWN. CRC correctness is
board-reported, not independently recovered from rounded printed measurements.

## Diagnostics and scope limits

General STATUS during early timer wake prints uninitialized zeros, while
NVS-backed autonomous status and records show exp=3; held STATUS restores the
proper checkpoint. Tracked separately in BACKLOG. Immediate post-upload status
was inside the cold-boot maintenance window (armed, not yet running); boot 36's
new records prove subsequent initialization. HOLD/RELEASE each warned reset
readback ENERGY=0 CHARGE=-1; exact cause is unmeasured and this is not a storage
error. Damaged-log recovery, open-failure diagnostics, real accumulator overflow,
storage-full behavior, lease expiry and full unattended timing remain untested
on hardware by this smoke. No corruption, clear, whole-flash erase or standalone
reset occurred. Uploader's inherent bootloader reset/program/restart did occur.

Two CODE processes were present; the user confirmed both idle. No host logger
ran; no held session remains. No firmware/test/dependency source changed, and no
commit or push was made. Detailed chronology and checkpoint are in LAB_NOTES and
CURRENT_STATE. Software gates were not repeated solely for documentation.
