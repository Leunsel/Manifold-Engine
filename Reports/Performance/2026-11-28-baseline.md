# Performance Baseline

Measured on 2026-11-27 and 2026-11-28, before 1.1.0. These numbers are the reference for later changes: a change that makes one of them worse needs a reason, and a claim that something got faster needs a run against the same setup (see `Docs/TESTING.md`).

All runs: Release x64 build, a 13th-generation Intel desktop CPU (8 performance and 16 efficient cores), Windows 11, a 144 Hz display. Every build was run from its own folder with its own settings. Times are medians unless said otherwise.

## Scanning

A small scratch program (not in the repository) commits about 1 GB of heap with fixed values. One `Tests.exe --compat --process=<it> --eval=<script>` per build runs the same Lua scans against the same running instance, so every build sees the same bytes. 4-byte values, writable memory, aligned. The builds were run alternately, five rounds each.

| Scan | 1.0.0 | main, 2026-11-27 |
|---|---|---|
| Exact 100 (first scan) | 0.078 s | 0.077 s |
| Next scan: exact 100 | 0.064 s | 0.077 s |
| Unknown initial value | 0.110 s | 0.124 s |
| Next scan: unchanged | 0.483 s | 0.451 s |
| Next scan: unchanged, again | 0.545 s | 0.560 s |
| Next scan: exact 100, after the unchanged ones | 0.341 s | 0.372 s |

The last row was a real regression: bisected to `6568988`, which added obscured (ACTk) values and whose check was inlined into the inner loop of every type. `051455c` keeps it out of line: 0.354 s, within the spread of 1.0.0. The other differences lie within the run-to-run spread.

## Start-up and idle

The program started visibly with `--profile-report=<file> --profile-seconds=40`, three times per build, measured from outside (process times, private bytes, handle and thread counts) and from its own log ("first frame was ready N ms").

| | 1.0.0 | main, 2026-11-27 |
|---|---|---|
| First frame (warm) | 255–257 ms | 253–255 ms |
| Idle CPU | 0.45–2.16 % of one core | 1.29–1.56 % of one core |
| Private memory | 71.9–73.8 MB | 71.5–72.8 MB |
| Handles / threads | 549 / 61 | 549–551 / 61 |

The first start of each new build took 1–5 s before its first frame (1003 ms and 5210 ms). Later starts of the same build did not; most likely the first read of the new files by the system's virus scanner. Not shown.

About 54 MB of the private memory is the graphics driver's Direct3D 11 device.

## An hour attached

Main at `051455c`, visible and attached to the test target for 60 minutes, one sample a minute of both processes. After 30 minutes a Lua timer set the speedhack to 1.01, so the in-process agent went into the target.

| Minutes | CPU (one core) | Private memory | Handles | Threads |
|---|---|---|---|---|
| 1–11 | 0.97 % | 74.7 → 75.1 MB | 558 → 554 | 58 |
| 11–21 | 1.01 % | +0.32 MB | 554 → 552 | 58 → 57 |
| 21–31 | 1.02 % | +0.64 MB (agent goes in) | 552 | 57 |
| 31–41 | 0.87 % | +0.38 MB | 552 | 57 |
| 41–51 | 0.92 % | +0.04 MB | 552 | 57 |
| 51–59 | 0.89 % | +0.04 MB | 552 | 57 |

Private memory ends at 76.5 MB (peak 91.3 MB during start-up) and grows by 0.04 MB per 10 minutes at the end; handles and threads stay level. The target gains 3 handles, 1 thread and 1 module with the agent (0.85 → 1.02 MB private) and keeps them. Work per frame: 0.43 ms on average, 0.82 ms at most.

Not covered: the program in use. A run that repeats a fixed workload (Memory Viewer scrolling, scripts switched, scans, Structure Dissect) with timestamps and finer samples would be needed to tie a spike to an action.

## Switching a script record

A real table of a 64-bit game: a record with a module AOB scan (about 96 MB module), a detour and four custom commands from a Lua module, switched on and off from a cooperative Lua thread (the path a click takes, since scripts run in the background).

| | On | Off |
|---|---|---|
| 1.0.0 and up to `ad22728` | about 1000 ms | about 900 ms |
| `cf25d08` | 30 ms | 15 ms |

The AOB scan itself took 7 ms, and the same script assembled directly from Lua 13 ms. The rest was the script worker waiting for the main thread: about 140 trips per switch (symbol lookups, custom-command checks, custom commands), each served only at the main thread's next tick. `3e8bb32` skips the trip for lookups no Lua resolver can answer, `cf25d08` serves the trips while they come, and `58840b8` ends the switch when the worker is done instead of after one more timer step (15.6 ms); on the table that last one is not measured yet (the integration test shows 16 ms before, under 8 ms after).

While switching ten times, the largest gap between two engine ticks was 22 ms, against 36 ms in a second without switching (no input in the window). Right after a click, while the window draws every frame, it is not measured.
