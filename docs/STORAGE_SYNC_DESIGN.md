# Autonomous Storage and Sync — Design

Design for the next milestone: the ESP32 keeps a durable local telemetry log while nothing is attached, and a host later syncs it.

**Status: design only. Nothing here is implemented.** Sections marked CONFIRMED were verified against the installed toolchain, the current source, or the TI datasheet. Sections marked PROPOSED are recommendations that have not been accepted as decisions. Accepted decisions live in [DECISIONS.md](DECISIONS.md).

## The problem

The current system is host-tethered:

```text
INA228 -> ESP32 -> Serial -> Python logger -> data/*.csv
```

If the Python logger is absent, `CSV_SAMPLE` and `CSV_DATA` history is lost. The ESP32 persists experiment accounting state in NVS but keeps no telemetry backlog.

The target:

```text
INA228 keeps converting
  -> ESP32 wakes on a configurable interval
  -> reads the accumulated interval result
  -> writes a durable local record
  -> sleeps again

later: host connects -> asks for records after sequence N
  -> ESP32 streams -> host stores -> host ACKs
  -> only then does storage become reclaimable
```

---

## 1. Flash and partition layout — CONFIRMED

Verified against Arduino ESP32 core **3.3.11**, FQBN `esp32:esp32:XIAO_ESP32C3`.

From `boards.txt`:

```text
XIAO_ESP32C3.build.flash_size=4MB
XIAO_ESP32C3.build.partitions=default
XIAO_ESP32C3.upload.maximum_size=1310720
XIAO_ESP32C3.menu.PartitionScheme.default=Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)
```

The active table is `tools/partitions/default.csv`:

| Name | Type | SubType | Offset | Size (hex) | Size (bytes) | Used today |
| --- | --- | --- | --- | --- | ---: | --- |
| *(bootloader + partition table)* | | | `0x000000` | `0x9000` | 36,864 | yes |
| `nvs` | data | nvs | `0x009000` | `0x5000` | 20,480 | yes — experiment state, power-test flags |
| `otadata` | data | ota | `0x00E000` | `0x2000` | 8,192 | no |
| `app0` | app | ota_0 | `0x010000` | `0x140000` | 1,310,720 | yes — the firmware |
| `app1` | app | ota_1 | `0x150000` | `0x140000` | 1,310,720 | **no — never used, no OTA** |
| `spiffs` | data | spiffs | `0x290000` | `0x160000` | 1,441,792 | **no — entirely free** |
| `coredump` | data | coredump | `0x3F0000` | `0x10000` | 65,536 | no |

Total: 4,194,304 bytes, exactly 4 MiB, fully allocated with no gaps.

**Where "Maximum is 1310720 bytes" comes from.** That figure in every compile is not a generic ESP32 limit and not the flash size. It is the `app0` partition size, `0x140000`. The current firmware uses 1,004,945 bytes of it, 76%.

**Filesystem type.** No filesystem is mounted today. The core provides LittleFS at `libraries/LittleFS`, and its default partition label is `spiffs`:

```cpp
bool begin(bool formatOnFail = false, const char *basePath = "/littlefs",
           uint8_t maxOpenFiles = 10, const char *partitionLabel = "spiffs");
```

So **LittleFS mounts on the existing `spiffs` partition with no partition-table change at all.** The subtype name is historical; it does not force SPIFFS.

That matters for risk: leaving the table alone means `nvs` never moves, so experiment state cannot be disturbed by adopting storage.

### Capacity available

- **Today, no changes: 1,441,792 bytes.**
- **Switching to the stock `no_ota` scheme: 1,966,080 bytes** (`app0` grows to 2 MB, `app1` disappears). `nvs` stays at `0x9000`/`0x5000` and `app0` stays at `0x10000`, so experiment state survives the change.
- **A custom single-app table could reach ~2,752,512 bytes** by giving everything from `0x150000` to `0x3F0000` to data. This needs a project-local partition CSV.

`app1` is 1.25 MB of flash that this project has never used and has no plan to use. It is the obvious place to find room if room is needed.

---

## 2. Storage mechanism — PROPOSED

| | LittleFS | NVS | Raw partition ring buffer |
| --- | --- | --- | --- |
| Append behavior | file append; open/append/close per wake | key/value, not a log | direct offset write, O(1) |
| Flash wear | wear-levelled; `CONFIG_LITTLEFS_BLOCK_CYCLES=512` relocates hot metadata | wear-levelled, but every write rewrites a page | we implement it |
| Power-loss behavior | copy-on-write, atomic metadata commits | atomic per key | we implement it |
| Partial-write recovery | FS survives; our own record tail still needs validating | n/a | entirely ours |
| Overhead | ~10% plus per-file metadata | high per entry for time series | ~0 |
| Complexity | low, stock library | low but wrong shape | high |
| Reclaim old records | truncate/rotate files | delete keys | advance tail pointer |
| Mount cost per wake | scans metadata on mount — **unmeasured** | negligible | none |

**Recommendation for the first bench implementation: LittleFS.** It is in the core, needs no partition change, is power-loss resilient by design, and produces a file a host can pull and inspect. Simplicity wins for a first cut.

**Keep NVS for small state and checkpoints.** Experiment id, interval number, running totals, sequence high-water mark, last-acked sequence, and `autonomous_interval_seconds` all belong in NVS. Nothing about this design changes that.

**Likely long-term optimization: a raw partition ring buffer.** Not for capacity, for time. Section 7 shows that awake time dominates the power budget, and LittleFS mount plus metadata commit adds to awake time on every single wake. If that cost turns out to be large, a ring buffer with fixed 64- or 72-byte slots removes it entirely. **This needs measuring before it can be decided** — see Section 12.

---

## 3. Wake and storage cadence — PROPOSED

**60 seconds is the development default, not an architectural commitment.**

One authoritative setting governs it:

```text
autonomous_interval_seconds     default 60
```

Rules that follow from having one setting:

- No other 60-second constant may exist in the autonomous path. The existing `MEASUREMENT_INTERVAL_MS` belongs to the host-tethered logger and must not be reused as the autonomous cadence.
- Every record stores the **actual measured** interval duration, not the configured one. A log containing a cadence change, a late wake, or a reboot mid-interval stays correct and self-describing.
- The setting persists in NVS so it survives power loss.
- The architecture must permit a future `LOGGER INTERVAL <seconds>` command. Not implemented in this stint.

### ESP cadence is not INA cadence

These are separate concepts and conflating them is the main modelling error to avoid:

- The **INA228 converts continuously**, roughly every 202 ms with the current configuration (3 × 1052 µs conversions × 64 averages), and accumulates CHARGE and ENERGY in hardware the entire time.
- The **ESP32 wake cadence** only decides how often that accumulation is harvested into a record.

So a 15-minute wake interval does not mean 15-minute-resolution charge data. Integrated charge and energy remain exact regardless of cadence, because the INA228 never stopped integrating.

### The information tradeoff

| Longer interval | Effect |
| --- | --- |
| Integrated charge and energy | **Preserved exactly.** The INA228 accumulates through the whole sleep. |
| Average current and average power | Preserved as interval averages, derived from the accumulators. |
| V / I / P / temperature snapshots | **Degraded.** One instantaneous sample per interval, so a 15-minute record says almost nothing about what happened inside those 15 minutes. |
| Intra-interval solar variation | **Lost.** Cloud edges, partial shading, and inverter behavior are invisible; a heavily-fluctuating 15 minutes and a steady 15 minutes with the same total charge produce identical records. |
| Battery voltage excursions | **Lost.** A brief sag or spike between wakes is never seen. |

Longer intervals cost temporal resolution, not energy accounting. Whether that matters depends on the question being asked, which is exactly why cadence must be a setting rather than a constant.

---

## 4. INA228 wake sequence — CONFIRMED HAZARD

This is the most important finding in this audit.

### What the current firmware does

Verified by reading the source. The accumulators are read in exactly two places, [solar-logger.ino:3611](../Arduino/solar-logger/solar-logger.ino#L3611) and [:3625](../Arduino/solar-logger/solar-logger.ino#L3625), both inside `closeMeasurementInterval()`.

`setup()` never reads them. It calls `resetInaAccumulators()` at [solar-logger.ino:4100](../Arduino/solar-logger/solar-logger.ino#L4100).

**Therefore: every boot — including every deep-sleep timer wake — destroys the accumulated CHARGE and ENERGY without ever reading them.**

Today this loses nothing, because both deep-sleep power-test variants explicitly suspend interval accounting. In autonomous mode it would silently discard the entire sleep period's charge on every single wake, and every record would report near-zero charge. Nothing in the current code would say so.

### What is safe, and what is not

Verified against INA228 datasheet SLYS021A, Table 7-5 (CONFIG, address `0h`):

| Bit | Field | Effect |
| --- | --- | --- |
| 15 | RST | full system reset, all registers to defaults, self-clearing |
| 14 | RSTACC | **clears ENERGY and CHARGE to 0** |
| 13-6 | CONVDLY | initial conversion delay |
| 5 | TEMPCOMP | shunt temperature compensation |
| 4 | ADCRANGE | shunt full-scale range |

- `configureIna228()` writes `CONFIG = 0x0000`. Both RST and RSTACC are 0, so **it does not reset the accumulators.** Safe.
- `configureIna228()` also writes `ADC_CONFIG` and `SHUNT_CAL`. Neither resets accumulators. But the datasheet states that writing the MODE bits "will interrupt and restart triggered or continuous conversions that are in progress," so an ADC_CONFIG write discards the in-flight conversion — up to ~202 ms of accumulation at the current settings.
- `resetInaAccumulators()` reads CONFIG, ORs in `0x4000` (RSTACC), writes it back. This is the only thing that destroys accumulation, and it is correct in isolation — it preserves the other CONFIG bits.

`SHUNT_CAL` scales the CHARGE register's interpretation. Writing the same value 819 is harmless; changing it mid-accumulation would make already-accumulated counts ambiguous. Do not change it without resetting.

### Correct autonomous wake ordering — PROPOSED

```text
1.  capture esp_sleep_get_wakeup_cause()
2.  Wire.begin() — bus only, no device writes
3.  verifyInaIdentity() — MANUFACTURER_ID / DEVICE_ID, read-only, safe
4.  READ ENERGY and CHARGE            <-- the sleep interval's integral
5.  READ DIAG_ALRT (0Bh) — ENERGYOF bit 11, CHARGEOF bit 10, MATHOF bit 9
6.  READ VBUS / CURRENT / POWER / DIETEMP snapshot
7.  compute interval duration from RTC-retained elapsed time
8.  build the record, CRC it, append it durably
9.  NOW reset accumulators (RSTACC) for the next interval
10. checkpoint NVS only if needed
11. configure the next wake and deep sleep
```

Reads come before writes. Nothing touches ADC_CONFIG on a normal wake at all — the INA228 is already configured and already converting, so reconfiguring it would only discard an in-flight conversion.

Step 5 is not optional. `ENERGYOF` and `CHARGEOF` tell us the accumulator rolled over during a long sleep, which at long cadences is a real possibility. `CHARGEOF` clears when CHARGE is read, so it must be read in the same wake. An overflow that is not recorded is silent data corruption.

Step 3 must not become step 3-and-reconfigure. Validating identity is a read; reconfiguring is not.

**Reconfiguration belongs only on a cold boot**, where the INA228 really might be in an unknown state and the 2-second ADC settle really is needed.

### RSTACC self-clearing — CONFIRMED empirically, not documented

The datasheet explicitly says RST "self-clears" but says no such thing about RSTACC. The current firmware sets RSTACC and never clears it.

It evidently does self-clear, and the committed data proves it: in `data/intervals.csv`, `interval_charge_mAh` stays around 1.5 mAh per interval across 1,174 intervals rather than growing cumulatively, and `running_charge_mAh` advances by exactly the interval value each minute. If RSTACC were sticky, accumulation would either stop or never reset.

The autonomous implementation should print the CONFIG readback after a reset so this stays observable rather than assumed.

---

## 5. Record format — PROPOSED

Fixed-width, little-endian, versioned, CRC-protected. Binary, not CSV — the host converts.

| Offset | Field | Type | Bytes | Units / notes |
| ---: | --- | --- | ---: | --- |
| 0 | `magic` | uint16 | 2 | fixed marker, resync and sanity |
| 2 | `version` | uint8 | 1 | record schema version, starts at 1 |
| 3 | `flags` | uint8 | 1 | see below |
| 4 | `seq` | uint32 | 4 | global monotonic sequence |
| 8 | `running_charge_uAh` | int64 | 8 | experiment cumulative, µAh |
| 16 | `running_energy_uWh` | int64 | 8 | experiment cumulative, µWh |
| 24 | `experiment_id` | uint32 | 4 | |
| 28 | `boot_id` | uint32 | 4 | power-on session identifier |
| 32 | `session_elapsed_ms` | uint32 | 4 | ms since this session's cold boot |
| 36 | `epoch_s` | uint32 | 4 | Unix seconds, 0 when unknown |
| 40 | `interval_ms` | uint32 | 4 | **actual measured** interval duration |
| 44 | `bus_uV` | uint32 | 4 | microvolts |
| 48 | `avg_current_uA` | int32 | 4 | microamps, signed |
| 52 | `avg_power_uW` | int32 | 4 | microwatts, signed |
| 56 | `temp_mC` | int32 | 4 | millidegrees C |
| 60 | `interval_charge_uAh` | int32 | 4 | microamp-hours, signed |
| 64 | `interval_energy_uWh` | int32 | 4 | microwatt-hours, signed |
| 68 | `crc32` | uint32 | 4 | IEEE 802.3 over bytes 0..67 |

**Packed size: 72 bytes exactly.**

Every 4-byte field sits on a 4-byte boundary and both int64 fields sit on 8-byte boundaries, so the layout needs no padding and no unaligned access on RISC-V.

Range checks against the hardware: `bus_uV` covers the INA228's 85 V maximum in 85,000,000 counts; `avg_current_uA` covers ±2,147 A; `interval_charge_uAh` covers ±2,147 Ah per interval against roughly 1,500 µAh observed per minute; the int64 running totals cannot realistically overflow.

`flags` bits, IMPLEMENTED. These are the definitions the firmware actually writes:

```text
1-0  TIME_QUALITY       two-bit field, see Section 8
       0 = UNKNOWN        no clock set this session; epoch_s is 0
       1 = SYNCHRONIZED   clock set this session; epoch_s written from it
       2 = HOLDOVER       clock was set, but long enough ago that drift is
                          no longer bounded by the sync
       3 = reserved
  2  FIRST_AFTER_BOOT   0x04  first record of this session
  3  ACCUM_SUSPECT      0x08  accumulators may not cover the full interval
  4  INTERVAL_ODD       0x10  interval_ms differs materially from the cadence
  5  INA_MATHOF         0x20  DIAG_ALRT MATHOF was set; current/power suspect
  6  INA_ACCUM_OF       0x40  DIAG_ALRT ENERGYOF or CHARGEOF was set
  7  EXPERIMENT_UNKNOWN 0x80  experiment_id could not be determined
```

Every value observed in the first hardware runs decodes from this table. `flags=0x4` is `FIRST_AFTER_BOOT` with `TIME_QUALITY = UNKNOWN`, which is the first record written after each boot. `flags=0x0` is every later record in the same session: time quality is still `UNKNOWN`, and `UNKNOWN` is zero, so it contributes nothing to the byte. Neither value indicates a problem.

`LOGGER STORAGE DUMP` now prints the decoded names alongside the hex, so a dump no longer needs this table to read.

**Bit 7 changed meaning.** It was proposed as `SEQ_GAP`, was never implemented, and was never set in any stored record. A sequence gap is already directly visible by comparing consecutive `seq` values in the log, so the bit carried nothing that could not be recovered another way. Whether a record's experiment attribution is trustworthy cannot be recovered any other way, so bit 7 now carries that. No stored record has bit 7 set, so no existing record changes meaning.

When `EXPERIMENT_UNKNOWN` is set, `experiment_id` holds `0xFFFFFFFF` rather than a plausible number. The flag is authoritative; the sentinel exists so a record that slips past a host's flag check still cannot be silently counted into experiment 0.

`TIME_QUALITY` is a field rather than a single valid/invalid bit because "the clock was right when this was written" and "the clock was set an hour ago and has been drifting since" are different claims, and a host that cannot tell them apart will silently treat one as the other.

There is deliberately no `DERIVED` value. The device never writes a timestamp it did not have; retrospective anchoring is a host-side operation and is marked in the host's store, not in the device record. See Section 8.

`epoch_s` is an unsigned 32-bit count of seconds since the Unix epoch, so it saturates in 2106 rather than 2038. That is adequate here and keeps the record at 72 bytes; widening it is a schema-version change if it ever matters.

If a raw ring buffer is adopted later, trimming to **64 bytes** would give exactly 64 records per 4096-byte sector. The two int64 running totals are the obvious candidates — they are derivable by summation within an experiment. That is a later optimization, not a first-cut concern.

---

## 6. Capacity — CONFIRMED arithmetic on CONFIRMED partition sizes

Assuming **90% of the partition usable** after LittleFS superblocks, metadata pairs, and per-file overhead. This is a planning figure; the true number should be measured once a file exists.

| Partition option | Raw bytes | Usable at 90% | Records at 72 B |
| --- | ---: | ---: | ---: |
| Current `default` | 1,441,792 | 1,297,613 | 18,022 |
| Stock `no_ota` | 1,966,080 | 1,769,472 | 24,576 |
| Custom single-app | 2,752,512 | 2,477,261 | 34,406 |

Days of history:

| Cadence | Records/day | `default` | `no_ota` | Custom |
| --- | ---: | ---: | ---: | ---: |
| 1 minute | 1,440 | **12.5 days** | 17.1 days | 23.9 days |
| 5 minutes | 288 | 62.6 days (2.1 mo) | 85.3 days (2.8 mo) | 119.5 days (3.9 mo) |
| 15 minutes | 96 | 187.7 days (6.2 mo) | 256.0 days (8.4 mo) | 358.4 days (11.8 mo) |

**One-minute records are not "already plenty."** On the current partition table they give under two weeks. That is comfortable for bench work and thin for a car that might sit unvisited for a month.

The cadence setting is what makes this tractable: bench at 60 s, and if the installed deployment needs months of autonomy, change one setting rather than the architecture.

---

## 7. Flash wear and write strategy — CONFIRMED arithmetic

At 1 record/minute:

```text
72 B × 1440/day          = 103,680 B/day
                         = 37,843,200 B/year
÷ 4096 B/block           = 9,239 block erases/year
÷ 352 blocks (default)   = 26 erases per block per year
```

Against a typical 100,000-cycle NOR flash endurance that is roughly **3,800 years**. Even allowing an order of magnitude for metadata write amplification it stays far beyond the hardware's life.

**Flash wear is not a constraint at any cadence under consideration.** It should not shape the design.

The one place wear concentrates is the file's LittleFS metadata pair, which commits on every append. The core already mitigates this: `CONFIG_LITTLEFS_BLOCK_CYCLES=512` makes LittleFS relocate metadata pairs after 512 erase cycles.

Options considered, and what each loses on sudden power loss:

| | Loses on power loss | Verdict |
| --- | --- | --- |
| **A. Durable write every wake** | at most the record being written, caught by CRC on recovery | **Recommended.** Wear is a non-issue, so buying anything with durability is a bad trade. |
| B. Buffer N records in RTC RAM, flush periodically | all buffered records, and RTC RAM does not survive power loss — so a car battery disconnect loses up to N intervals | Rejected for the first cut. Trades real data for a resource we are not short of. |
| C. Page/block-sized batches | up to one block of records | Same objection as B. |
| D. Fixed-size flash ring buffer | at most the record being written | Attractive later, for **awake time**, not for wear. |

### Awake time is the real power constraint

Using the measured figures — 28.4 mA awake, 1.07 mA in deep sleep with the INA228 converting:

| Cadence | Average current, 3.5 s wake (current boot path) | Average current, 0.3 s wake (optimized) |
| --- | ---: | ---: |
| 60 s | **2.66 mA** | 1.21 mA |
| 300 s | 1.39 mA | 1.10 mA |
| 900 s | 1.18 mA | 1.08 mA |

Deep-sleep floor is 1.07 mA.

**The current 3.5-second boot path costs more than the sleep it enables.** At 60-second cadence it takes the system from 1.07 mA to 2.66 mA — worse than 2.5× the floor, and it makes cadence look like the dominant variable when it is not.

That 3.5 seconds is `delay(1500)` for USB CDC enumeration plus `delay(2000)` for ADC settling. In an installed car both are wrong:

- USB is not connected, so nothing needs to enumerate.
- The INA228 has been converting continuously through the sleep, so it is already settled.

An autonomous wake path that skips both, does its reads, appends, and sleeps should be a few hundred milliseconds. **This is why the wake path cannot simply reuse the existing `setup()`,** and it is a stronger argument for restructuring than storage was.

---

## 8. Timekeeping — PROPOSED

**Wall-clock time is synchronizable state, not a guarantee.** The logger must produce correct, ordered, durable records whether or not it has ever been told what time it is. Everything below follows from that.

There is no external RTC in this project and the first implementation does not add one.

### What survives what — CONFIRMED

| | Deep-sleep wake | Cold power loss |
| --- | --- | --- |
| `RTC_DATA_ATTR` variables | **retained** | lost |
| System wall-clock (`gettimeofday`) | **retained**, see below | lost |
| `millis()` / normal RAM | lost | lost |
| NVS | retained | retained |

RTC-retained variables are already proven in this project; the deep-sleep power test uses a magic-guarded `RTC_DATA_ATTR` cycle counter.

For the system clock, the installed build is configured to carry it across deep sleep:

```text
CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y
CONFIG_ESP32C3_TIME_SYSCALL_USE_RTC_SYSTIMER=y
```

That pairing means `gettimeofday()` is backed by the RTC counter for persistence across sleep and by the high-resolution timer for resolution while awake, and ESP-IDF exposes `esp_set_time_from_rtc()` to restore it on wake. Both halves live in the RTC power domain, which is why the clock survives deep sleep and does not survive a power cut.

**Verified as configuration, not as behavior.** The build is set up for this; it has not been tested on this board. Section 12 lists the bench check. Until that passes, the implementation should treat retained wall-clock as expected-but-unproven and keep the sequence number as the load-bearing mechanism.

### The clock source is an RC oscillator — CONFIRMED

```text
CONFIG_RTC_CLK_SRC_INT_RC=y
CONFIG_RTC_CLK_CAL_CYCLES=576
```

RTC slow clock is the **internal RC oscillator**, not an external 32.768 kHz crystal. IDF calibrates it against the main crystal at boot over 576 cycles, which helps considerably, but this is not precision wall-clock hardware and nothing in this project should describe it as such. RC oscillators drift with temperature and supply voltage, and a logger sitting in a car sees both.

Drift on this board is **unmeasured**. No absolute-time accuracy figure may appear in the docs, the firmware output, or the host tooling until it is.

### Time quality, not a valid bit

Every record carries a two-bit `TIME_QUALITY` field:

| Value | Meaning | `epoch_s` |
| --- | --- | --- |
| `UNKNOWN` | no clock has been set this session | 0 |
| `SYNCHRONIZED` | clock set this session, recently enough to trust | real |
| `HOLDOVER` | clock was set this session, but long enough ago that drift is no longer bounded | real, degrading |

The device tracks the session-elapsed time at which the clock was last set and promotes `SYNCHRONIZED` to `HOLDOVER` after a configurable threshold. That threshold is a guess until drift is measured, so it should be a named setting, not a literal.

`epoch_s` and the quality field always travel together. A host must never read `epoch_s` without reading the quality alongside it.

### Retrospective anchoring — host-side only

Records written before a sync are not lost causes. Every record carries `boot_id` and `session_elapsed_ms`, so a host that connects mid-session can anchor the whole session:

```text
wall_time(record) ≈ host_now − (session_elapsed_now − session_elapsed_record)
```

Three constraints on this, all of which matter:

- It works for the **current session only**, identified by `boot_id`. A power cut ends the session and destroys the anchor. Records from earlier sessions keep whatever quality they were written with.
- The accuracy is bounded by RC drift over the interval being spanned, plus whatever latency the transport adds. It is an estimate.
- **The device never rewrites its own records.** Anchoring happens in the host's store, and the host marks those timestamps as derived. A derived timestamp and a synchronized one must remain distinguishable forever, which is why `DERIVED` is not a device-side quality value — the device would be claiming something it never knew.

### Setting the clock

One logical operation, identical semantics on every transport:

```text
SET_TIME <unix_epoch_seconds>
```

| Transport | How it arrives | Status |
| --- | --- | --- |
| USB serial | the Python logger issues it automatically on connect | designed, not implemented |
| Wi-Fi | NTP, with the result fed into the same operation | longer-term |
| Bluetooth LE | mobile client supplies its own clock | longer-term |

The transport is irrelevant to the record semantics. Whatever sets the clock, the result is the same: `epoch_s` becomes real and `TIME_QUALITY` becomes `SYNCHRONIZED` for subsequent records.

**Automatic sync from the Python logger** is the first one worth building, and it is cheap: the logger already owns the serial port and already writes commands through its own connection, so it can issue `SET_TIME` immediately after a successful connect without touching the `send.sh` handoff protocol or the upload handshake. Re-syncing periodically during a long session also costs nothing and keeps `HOLDOVER` from being reached while a host is attached.

Setting the clock must be idempotent and safe to repeat. It must never reorder, rewrite, or invalidate records already stored.

### What we can and cannot claim

- **Ordering, within and across sessions: exact.** `seq` guarantees it and never depends on wall-clock time.
- **Interval duration: good.** Every record carries its own measured `interval_ms`.
- **Absolute time: only as good as the last sync, degraded by unmeasured RC drift, and absent entirely until a first sync.** Records say which of those applies to them.

### External RTC

Not required, not added. A battery-backed RTC is the only way to make absolute time survive complete loss of board power, and it becomes worth considering only if a measured drift figure, or a real need for wall-clock continuity across power cuts, turns out to justify the parts and the wiring. Recorded as a conditional longer-term option in [BACKLOG.md](BACKLOG.md).

---

## 9. Sync protocol — PROPOSED

Transport-independent. Record semantics stay fixed while transport changes from USB serial to BLE or Wi-Fi.

```text
INFO
  -> newest_seq, oldest_seq, stored_record_count,
     last_acked_seq, bytes_used, bytes_total,
     autonomous_interval_seconds,
     boot_id, session_elapsed_ms, epoch_s, time_valid,
     dropped_unacked_count

SYNC FROM <n>
  -> streams every stored record with seq > n, in ascending seq order

ACK <n>
  -> host asserts every record through seq n is durably stored

SET_TIME <unix_epoch_seconds>
  -> sets the clock; subsequent records carry TIME_QUALITY = SYNCHRONIZED
  -> identical semantics over USB serial, BLE, or Wi-Fi/NTP
  -> idempotent; never rewrites or reorders stored records
```

`INFO` reports `last_sync_session_ms` alongside `session_elapsed_ms`, so a host can compute how stale any record's timestamp was at the moment it was written without the record needing to carry that separately.

Rules:

- **ACK is cumulative and highest-contiguous.** A host that receives 100–150 but is missing 120 acknowledges 119, never 150.
- **Retransmission is always safe.** `SYNC FROM` is a pure read; it changes no device state.
- **Duplicate delivery is harmless.** `seq` is the idempotency key; a host that already holds a record discards the copy.
- **Transmission never deletes anything.** Only an ACK makes a record reclaimable, and reclamation is a separate step from acknowledgement.
- **`last_acked_seq` persists in NVS.** One write per sync session, not per record.

### When storage fills before a sync

This needs an explicit policy, and the two candidates lose different things:

- **Stop logging.** Preserves the oldest unsynced data, and the device goes silent exactly when nobody is watching. Recent data — usually the most interesting — is the data lost.
- **Overwrite oldest unacked.** Keeps the log current. Loses the oldest unsynced records.

**Recommended: reclaim acked records first; if still full, overwrite the oldest unacked record and count it.** A persistent `dropped_unacked_count` appears in `INFO`, the event is printed loudly when it happens, and the first record after a drop carries a flag. Silence is the only unacceptable option; either policy is defensible as long as it is announced.

This is a policy choice that should be confirmed rather than assumed.

---

## 10. Global sequence number — PROPOSED

Requirements: monotonic across reboot and power loss, no NVS write per record, and **duplicates are worse than gaps.**

Two mechanisms together:

1. **Recover from the log tail on boot.** Records are fixed-length, so the last record is at a known offset. Seek to the end, read the final 72 bytes, validate magic, version, and CRC, take its `seq`. This is O(1), not a scan.
2. **Reserve blocks ahead in NVS.** Keep `seq_high_water` in NVS. Before using a block of sequence numbers, write `seq_high_water = current + 256`. On boot, `next_seq = max(log_tail_seq, nvs_high_water) + 1`.

Reserving ahead is what makes crashes safe. A crash inside a reserved block means the block is abandoned and the next boot starts past it — a **gap**, never a reuse. Gaps are visible in the data and harmless; duplicate sequence numbers would silently corrupt a host's database.

Cost: one NVS write per 256 records. At 60-second cadence that is one write per 4.3 hours.

---

## 11. Crash and partial-write recovery — PROPOSED

Every record is fixed-length with a magic, a version, and a trailing CRC32, which is what makes the tail recoverable.

Boot recovery:

```text
1. mount; if mount fails, say so loudly and do not silently format
2. stat the log; if size % 72 != 0, the tail is a partial write:
     truncate to the last whole-record boundary
     print exactly how many bytes were discarded
3. read the final record; validate magic, version, CRC32
4. if invalid:
     walk backwards one record at a time until a valid record is found
     truncate everything after it
     print exactly how many records were discarded and why
5. take next_seq from the recovered tail, reconciled with the NVS high-water mark
6. if no valid record exists at all, say so explicitly and start from the NVS mark
```

Non-negotiables:

- A corrupt record is never handed to a host as if it were good.
- Truncation is always reported with a count, never performed quietly.
- A failed mount never triggers an automatic format. Formatting destroys unsynced history and must be an explicit operator action.

---

## 12. Open questions that need a bench test before coding

These are the things this design could not settle by reading, and each one could change a recommendation.

1. **LittleFS mount + open + append + close time per wake.** Section 7 shows awake time dominates the power budget. If this costs hundreds of milliseconds, the raw ring buffer moves from "later optimization" to "first implementation." Measure before committing.
2. **Minimum achievable autonomous wake duration.** How short can boot get with the USB delay and ADC settle removed on a timer wake? The 0.3 s figure in Section 7 is an assumption, not a measurement.
3. **RTC drift on this board.** Compare commanded sleep duration against host `captured_at` deltas over many cycles, ideally at more than one ambient temperature since the clock source is an RC oscillator. Determines the `HOLDOVER` threshold, whether retrospective anchoring is worth anything, and whether an external RTC is ever needed. **No absolute-time accuracy claim may be published until this is done.**
4. **Whether wall-clock actually survives deep sleep on this board.** The build is configured for it (`CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y`), but that is configuration, not tested behavior. Set the clock, sleep several cycles, read it back, compare against the host. Until this passes, treat retained wall-clock as expected-but-unproven and keep `seq` load-bearing.
5. **Real LittleFS usable fraction.** The 90% figure is a planning assumption. Create a file, fill it, and read back actual `totalBytes()` and `usedBytes()`.
6. **Accumulator behavior across a long sleep.** Does CHARGE or ENERGY overflow at 5- or 15-minute intervals at realistic solar currents? `DIAG_ALRT` answers this directly and cheaply.
7. **RSTACC readback.** Confirm from the CONFIG readback that the bit self-clears, rather than continuing to infer it from interval data.

---

## 12a. Bench prototype — IMPLEMENTED, storage CONFIRMED on hardware

A bench-only prototype of one sleep/read/store cycle exists in the firmware and has now run on hardware.

**Storage is confirmed.** The first run on 2026-09-11 produced eight 72-byte records, a 576-byte log, sequences 1 through 8, and an intact tail on a later cold boot. Read-before-reset ordering, durable append, CRC and tail validation, experiment/storage isolation, and the refusal to auto-format all held.

**Timings are still unmeasured.** The instrumentation was measuring the wrong span and reported a 0.014 ms total for a multi-second cold boot. It is fixed but has not been re-run, so every figure Section 12 waits on is still open.

It is a bench test, not production behavior. No Bluetooth, no Wi-Fi, no sync, no ACK, no pruning, no ring buffer.

### Commands

```text
LOGGER AUTONOMOUS ON        enable autonomous sleep/wake/store mode
LOGGER AUTONOMOUS STATUS    mode, cadence, storage, session state
LOGGER AUTONOMOUS OFF       disable it; stored records are NOT deleted
LOGGER INTERVAL <seconds>   the one authoritative cadence setting, 10-3600
LOGGER STORAGE INFO         filesystem and durable-log summary
LOGGER STORAGE DUMP         decode stored records to Serial
LOGGER STORAGE CLEAR YES    erase stored records, never experiment state
```

The armed flag and the cadence persist in NVS (`auto_test_arm`, `auto_int_s`), so the test survives USB disconnect and running from the AA pack. It is mutually exclusive with both power tests in both directions.

### Cold-boot maintenance window — added after the first hardware run

An armed board could not be stopped over USB. `handleSerialCommands()` runs only from `loop()`, and an armed cold boot returned from `setup()` into deep sleep without reaching it, so a delivered `LOGGER AUTONOMOUS OFF` was never parsed. Reflashing was the only remaining recovery.

On a true cold boot the firmware now opens a window of `AUTONOMOUS_COLD_BOOT_MAINTENANCE_MS` (15 seconds during development) before it resumes autonomous sleeping. It announces itself, counts down every 5 seconds, and parses commands normally throughout. `LOGGER AUTONOMOUS OFF` inside the window clears the persisted flag and prevents deep sleep. Commands sent during the preceding initialization are buffered by the USB CDC driver and parsed when the window opens, so they are not lost.

A timer wake does not open a window, because awake time dominates the power budget and every reset already provides the recovery path.

A stop that is requested but cannot be persisted also prevents deep sleep. Resuming autonomous operation after an explicit stop request is the same failure the window exists to prevent, so the firmware refuses to sleep and states that the flag is still armed.

This is a development recovery mechanism, not the final answer. An explicit maintenance button or a BLE management mode remains in [BACKLOG.md](BACKLOG.md).

### Autonomous lifecycle with USB rendezvous - IMPLEMENTED, rendezvous UNPROVEN

```text
        cold boot (armed)
             |
   15 s maintenance window  --HOLD-->  HOST SESSION
             |                              ^
             v                              |
        DEEP SLEEP                          |
             |                              |
        timer expires                       |
             v                              |
   TIMER_WAKE_MEASUREMENT                   |
     read NVS experiment context            |
     validate INA (reads only)              |
     read CHARGE / ENERGY                   |
     read DIAG_ALRT                         |
     snapshot                               |
     append durable record                  |
     reset accumulators                     |
             |                              |
             v                              |
     USB_RENDEZVOUS (10 s)  ----HOLD--------+
             |                              |
        no claim                     KEEPALIVE every 5 s
             |                              |
             v                       RELEASE or lease expiry (15 s)
        DEEP SLEEP  <-------------------- close open tethered interval
```

The timer wake, the durable storage, and the USB rendezvous are all **confirmed on hardware** as of 2026-09-11: a timer wake exposed USB with no physical intervention, a host claimed it, and release returned the board to sleep. Lease expiry and host-session interval accounting were broken in that first run and are fixed but not yet re-tested; see [LAB_NOTES.md](LAB_NOTES.md).

Sleep policy asks `anyHostConnected()` against a transport bitmask, never USB directly, so Wi-Fi and BLE can hold the board awake later without the state machine changing. Electrical presence never counts as a claim; only the explicit lease does. See [DECISIONS.md](DECISIONS.md) D-025 and D-026.

These are development values. A 10-second rendezvous on a 60-second cadence spends a sixth of the duty cycle awake, which is accepted while the behavior is being proved and is the first thing a deployment mode would remove.

### Sequence reservation, corrected

The first run ended with the next sequence at 131 for a log holding eight records. Blocks of 64 are reserved, the next sequence was always taken as one past the reservation, and every arm and cold boot re-reserved unconditionally, so each restart consumed 65 numbers.

An intact log tail is now the sequence authority, since nothing but an append writes a record. A clean restart continues from the log. The NVS reservation remains the floor whenever the log is empty, partial, corrupt, or out of order, which preserves gap-over-reuse in exactly the cases that need it. A reservation is written only when the current one does not already cover the next sequence.

Once pruning exists this stops holding, because a pruned log no longer carries the highest sequence ever used. The NVS floor will have to apply unconditionally again.

### Accounting isolation — option B

The running charge and energy totals in these records are **test-local**. They live in RTC memory, are recovered from the durable log on cold boot, and are not the NVS experiment totals.

The autonomous path never calls `saveCheckpoint()` and never modifies `experimentId`, `completedInterval`, `runningCharge_mAh`, or `runningEnergy_mWh`. Each record carries `experiment_id` only as read-only context. Experiment 2's accounting cannot be double-counted or erased by this test.

Interval accounting and machine-readable CSV output are suspended while the test runs, for the same reasons as the deep-sleep power test.

### Timer-wake path

The branch happens at the top of `setup()`, before any cold-boot initialization, and skips both development delays:

- `delay(1500)` for USB CDC enumeration — nothing to enumerate on battery.
- `delay(2000)` for ADC settling — the INA228 stayed powered and converting through the whole sleep, so its ADC never stopped. The datasheet's 60 µs start-up figure applies to leaving shutdown mode, which is not what happened.

Cold-boot behavior is unchanged.

### Deviation from the design's record semantics

One, and it is deliberate: `running_charge_uAh` and `running_energy_uWh` carry **test-local** totals rather than experiment cumulative totals, because of the accounting isolation above. The field layout, byte offsets, and 72-byte size are exactly as specified in Section 5.

### What still needs hardware

Everything in Section 12 remains open. **No timing value has been recorded yet**, because the first run's instrumentation was wrong.

Timing output is now split so the answers are readable: `Wake -> I2C ready`, `INA accumulated read`, `INA snapshot`, `LittleFS mount`, `Record append total` broken into open / write / flush+close, `Accumulator reset`, `Total work (no Serial)`, and a total whose label names the path it measured. Storage scan and recovery time is reported on the cold-boot path, which is the only path that scans.

`LittleFS mount` and the append sub-timings are what decide question 2, the storage mechanism.

---

## 13. Recommended first implementation milestone

Deliberately narrow. Storage first, sync second.

1. Mount LittleFS on the existing `spiffs` partition. No partition-table change.
2. Add `autonomous_interval_seconds` in NVS, default 60, as the single cadence authority.
3. Implement the 72-byte record with CRC32 and append it — while still host-tethered and awake. No deep sleep yet.
4. Implement boot recovery and tail validation, with loud diagnostics.
5. Implement sequence recovery with NVS block reservation.
6. Add `INFO` and `SYNC FROM` over the existing serial command path. `ACK` and reclamation come after.
7. Only then restructure the wake path for autonomous deep-sleep operation, using the ordering in Section 4.

Steps 1–6 are testable at a desk with the existing tooling and cannot lose experiment data. Step 7 is where the accumulator hazard becomes live, and it should not be attempted until the record format and recovery are proven.

---

## 14. Experiments versus storage

The distinction is a requirement, not a preference:

- **Experiment** — a logical measurement grouping, identified by `experiment_id`.
- **Storage** — durable telemetry history, ordered by `seq`.

`RESET YES` may increment `experiment_id` as it does today. It must **not** erase stored telemetry, reset the global sequence, or reset acknowledgement state. Storage reclamation is a separate operation with its own trigger.

Recorded as an accepted decision in [DECISIONS.md](DECISIONS.md).

---

## 15. Explicitly out of scope

**Adaptive day/night cadence** stays a longer-term idea in [BACKLOG.md](BACKLOG.md). It is not a requirement for the first implementation and must not shape it. The one thing this design owes it is that a configurable cadence makes it possible later without a rewrite.
