# Hauling history-check evidence — 23 October 2026

Active hauling exposed long world-simulation steps during station production. CPU samples led to `production_select_loose_shell`, then cargo origin lookup, then signature checks over the whole station history. Each appended event caused the origin index to read and verify that history again.

The fix keeps the full history scan and reuses successful signature checks for exact header bytes. The header includes the public key, payload hash, link, event ID and signature. Each scan checks authority, record order, links and payload bytes before using a cached signature. Existing file snapshot, registry and station-tail checks still apply. The ordinary verifier keeps its full signature-check path.

The final cache holds 2,048 headers in 512 buckets with four slots each. It uses 379,408 bytes, below its 384 KiB bound. A hit compares all 184 header bytes. A miss performs the signature check. Failed signatures stay out of the cache. Each caller owns its cache and provides single-thread access.

## Measured route

The first run used the v0.6.0 packaged web build and release media. Its active towing steps reached 99.2 ms. The paired trace then used two diagnostic builds from the same Emscripten 5.0.4 toolchain, with `SIGNAL_SIM_PROFILE=ON` and `--profiling-funcs`. Both used the same release media, seed 2037, a fresh browser context and a 1280 × 720 viewport on an Apple M4 Max with 14 logical CPUs.

The baseline source was `94588d6b6594570bdecbc78602d4bb2d7e112abc`. The updated diagnostic source was `d4a24aa5579b3282b0f239e8aba2bb6b1ddac4be`, which used **1,024 cache entries**. These timing measurements cover that first cache size. The final 2,048-entry choice has the separate check-count comparison below.

A setup hook placed a rock and a loaded pod beside the ship. Ordinary Space input attached them. The route then held forward, turned near the station, made station contact, and coasted. The rock remained attached through the recorded phases. The pod was released or consumed during the straight phase. The route also triggered the Hauler episode from the release assets; its playback covered part of one phase. The fresh route had 12 seconds at dock, then 52 seconds across idle tow, forward flight, turning, the media trigger and coast.

| Route measure | Baseline | Cache with 1,024 entries |
| --- | ---: | ---: |
| Fresh active hauling: largest authority step | 104.1 ms | 37.2 ms |
| Fresh active hauling: largest frame processing time | 106.1 ms | 37.7 ms |
| Fresh active hauling: dropped simulation ticks | 64 | 0 |
| Aged hauling: largest authority step | 118.1 ms | 2.8 ms |
| Aged hauling: largest frame processing time | 124.1 ms | 14.9 ms |
| Aged hauling: dropped simulation ticks | 4 | 0 |

The aged run advanced the docked world by 600 extra simulated seconds. Normal frames also ran during warm-up, so the recorded world age was about 640 seconds before the 24-second hauling route. This is a passively aged starter world. The updated aged run recorded 149 fresh signature checks and 786 reused checks across 14 complete history scans. The fresh updated run recorded 107 fresh checks and 341 reused checks.

Frame processing time measures game work inside a frame. Display cadence is stored separately as `animationFrameIntervalMs` in the fresh trace. The figures above describe these short local runs. User applications remained active. Other agents paused their work for the baseline and aged samples; the first part of the updated fresh sample overlapped a final build/browser run and a native test process. Wall-clock controls and changing production events also produce small differences between the routes.

Cold histories still need signature checks. The first docked history scan took about 48 ms in the updated trace. Cache capacity and bucket collisions also cause fresh checks in larger histories. These are useful next profiling targets. The full-hour gameplay review remains a separate check.

## Size choice and safety checks

A deterministic native fixture creates four valid station histories with 692, 221, 216 and 292 records. These match the live record counts observed during this review. It reads all 1,421 records twice and compares cached verification with the ordinary verifier, including the complete result report.

| Cache entries | Fresh checks on the second read | Reused checks | Cache bytes |
| --- | ---: | ---: | ---: |
| 1,024 | 1,136 | 285 | 189,712 |
| 2,048 | 420 | 1,001 | 379,408 |

The larger size cuts repeat signature work by 63% in this fixture. The test keeps a check-count bound for this workload and tests eviction beyond the chosen capacity. Separate tests compare appended records, changed payloads, changed signatures, different authorities, truncated histories, rewritten histories, forked links and clean segment resets against the ordinary verifier. Existing origin-cache tests cover file and registry changes and snapshot races.

Run the focused browser route with:

```sh
SIGNAL_HAUL_PROFILE_SECONDS=12 npm run smoke -- tests/gameplay-profile.spec.ts --grep 'active hauling' --workers=1
```

The default is three seconds per phase. Each phase accepts up to twelve seconds. This short route runs in normal browser CI alongside the fresh/saved dockside profile. It produces screenshots, complete frame reports, tow state and signature-work counts. `SIGNAL_JANK_PROFILE_SECONDS` continues to control the longer dockside samples separately.

Raw reports, build hashes and phase timing are in [comparison.json](evidence/hauling-signature-cache-2026-10-23/comparison.json). The simulation phase logs are beside it. Rendered turn and station-contact frames were also inspected locally. The browser check attaches fresh screenshots for each route phase.
