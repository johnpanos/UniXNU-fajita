# Graphics: the gfx suite, its baseline and the burn-down (iOS 16.7.16 on fajita)

The suite measures what the panel shows, frame by frame, in every scene that moves, and says per scene what bounds it. It replaces
fps.sh's two numbers as the measure for graphics work (fps.sh stays as the quick check AGENTS.md names).

## Running it

```sh
# a held session with Wi-Fi (the web scenes need it); without Wi-Fi run only the first five scenes (session.sh start)
WIFI_HOLD=1 WIFI_EXTRA_ARGS="fajita-wifi-iokit=1 fajita-wifid=1" scripts/fajita/test/bg.sh start NAME --max 1500 \
    scripts/fajita/test/wifi-test.sh modem link servers wlfw scan assoc dhcp ping
scripts/fajita/build-usbdbg-agent.sh $FAJITA_OUT/tools/fjdbg-agent       # once: ngpu-metal (cpu sampler) and fjbundle into $FAJITA_OUT/tools
KM=<the running kernel.macho> scripts/fajita/test/bg.sh start gfx --max 1300 scripts/fajita/test/gfx-suite.sh RUN [SCENE...]
#   -> .cache/fajita-work/gfx/RUN/report.txt, report.tsv, metrics.json, the scene JSONs, rest screenshots, frametrace/
REPEAT=2 ... gfx-suite.sh RUN            # every scene twice (SCENE, SCENE-r2) and noise.txt: the run-to-run spread
REBASE=1 ... gfx-suite.sh RUN            # save the rest screenshots as references (.cache/fajita-work/gfx/refs; iOS UI, never in git)
ENV=gpu-env-file ... gfx-suite.sh RUN    # extra gpu-env lines for the run (backboardd restarted once to read them)
scripts/fajita/test/gfx-suite.sh ab RUN ENV_A ENV_B [ROUNDS]   # A/B two gpu-env variants in one boot, interleaved; RUN-ab.txt
python3 tools/gpu/gfx-report.py --compare RUN_A_DIR RUN_B_DIR  # two runs side by side (two builds)
```

KM must be the kernel of the running build (wifi-test.sh builds into .cache/fajita-out-wifi: its
`unixnu/fajita-ios16/kernel/kernel.macho`): the tools read kernel rings by symbol. A full run (9 scenes) takes about 5 minutes,
REPEAT=2 about 9; a session ends after 720 s when someone else is queued, so run subsets (`gfx-suite.sh RUN home applib settings`)
when the phone is busy.

### Scenes

| scene | what moves | steps |
| --- | --- | --- |
| home | home pages 1 <-> 2 | 4 plain swipes, 4 60-frame swipes |
| applib | App Library list (blurred backdrop) | 8 60-frame drags up and down |
| settings | Settings list | 4 slow drags, 2 short flings, the home gesture |
| calendar | Calendar launch (open), day view, day swipes | launch, 4 drags, left/right swipes, home gesture |
| maps | Maps launch, map pans | launch, 4 pans, home gesture |
| web-hn, web-wiki, web-apple | Safari scrolling news.ycombinator.com, en.wikipedia.org, www.apple.com | 3 drags down, 3 up |
| web-heavy | www.theverge.com (images, autoplaying media): 4 s of its own animation, then 6 drags | `anim,4`, drags, home gesture |

Photos and Notes are empty on this Data and the App Store cannot connect (no Apple ID), so they are not scenes. Apps are opened by URL
(`fjbundle --open`: calshow://, maps://, https://) because the home screen's layout changes as apps are installed.

## How it measures

- **Frames are the panel's.** FajitaDpu.cpp records the mach time of every VIG0 flip latch (`fajita_dpu_latch_ring`, 2048) and every
  PP0 rd_ptr (`fajita_dpu_vsync_ring`); LegacyDisplayProbe.cpp every retired swap (`native_legacy_retire_ring`). With the board's
  fajita-fb-flip=2 a latch is the frame start at which the panel begins showing a new frame. gfx-scene.py reads only the scene's
  entries over readv after the scene; a ring that wrapped is reported (`overflow`), never dropped silently.
- **Motion windows.** Each touch step (kcmd touch: the kernel's own start/end of the gesture) opens a window at its start (a tap or
  an `open`: at its end) that lasts while the finger is down, then while frames keep coming less than IDLE_MS (150) apart, for at
  most TAIL_MS (2000) after the lift. A gap of any length while the finger moves is a stall; a gap of over 150 ms after the lift ends
  the animation. Idle frames (the home screen's clock at 10 Hz) are never counted. `anim,S` is a window without touch.
- **Speed metrics** (per scene, over its windows): frames, fps (intervals / their sum), frame time p50/p99/p99.9, 1% and 0.1% lows
  (fps of the mean of the slowest 1% / 0.1% of frames, at least one frame: with under 1000 frames the 0.1% low is the worst frame),
  frames over 1.5 vsyncs (">16.7") and over 2.5 (">33"), missed vsyncs (round(interval / period) - 1, summed), the longest frame, and
  resp (touch-down to the first new frame, median; iOS's gesture recognition included).
- **GPU:** the clock governor's 20 ms samples (`fajita_gpu_gov_ring` in FajitaGpuProbe.cpp: RBBM busy and always-on cycles, DCVS
  level, cap, hottest tsens) over the motion windows: busy %, the real clock, levels, temperature. `kcmd gpu perf begin|end` (new,
  non-blocking) gives the a6xx counters over the scene: SP ALU share, SP stalls on TP, TP stalls on UCHE, VBIF latency.
- **CPU:** `ngpu-metal cpu 2 NAME...` back to back over the scene (package/gpu-tools/ngpu-metal.mm, extended): per core
  (host_processor_info), per process, and the named processes' threads with how often each was running or waiting when sampled
  every 20 ms and its priority. Windows are weighted by their overlap with motion.
- **Frame trace** (NGPU_FRAME_TRACE=1, on by default in the suite; TRACE=0 turns it off): per render-server frame, cpu_gap (from the
  previous swap offer until the frame's first command buffer: waiting on the previous present, a vsync or the app's commit),
  encode, commit (translation and submission), gpu, present (offer until the presenter's copy returned). The report shows the
  median frame and the mean slow frame (> 1.5 vsyncs).
- **Bound** per scene: vsync (>= 95% of frames on time); GPU (busy >= 85%, or GPU time the largest part of the slow frames);
  CPU THREAD (a thread running >= 85% of its samples); CPU render server (encode + commit the largest part of the slow frames);
  else pipeline (nothing saturated: the slow frames waited).
- **Correctness:** a screenshot at each `shot` rest point against the reference (`REBASE=1` saves them), below the status bar:
  pixels differing by over 8, DIFF above 1%. Web pages change by themselves (news, ads), so their diffs are only meaningful within one
  boot (the A/B mode).
- **Stability:** CRASH rows, swap_wait_gated timeouts, GPU faults and panics in the run's log (stability.json), and per scene the
  hand-off counters (reclaimed, retire_lost, flips refused, GPU fallbacks, scanned-buffer writes) and GPU IRQ errors.

## Baseline (ios16-port 7ebf612 + the suite, stack a43c5be, default boot args, frame trace on)

Run base2 (REPEAT=2: every scene twice, SCENE-r2 the second pass; session-093640, Wi-Fi up, 2026-10-26):

```
scene          frames   fps   p50   p99  p99.9 1%low 0.1%low >16.7  >33 missed    max  resp gpu%  MHz  bound
home              550  55.9  16.7  50.0   91.1  12.8   10.0    28    7     45  100.1    12   60  738  pipeline
applib            408  43.4  16.7 100.1  153.5   8.0    6.0    97   15    154  166.8    99   52  737  pipeline
settings          514  58.4  16.7  33.4   58.3  21.2   15.0    14    4     19   66.7    32   52  737  vsync
calendar          584  57.4  16.7  33.4   57.1  20.0   15.0    21    5     27   66.7    97   55  732  vsync
maps              213  40.3  16.7 114.7  130.0   8.2    7.5    71   13    101  133.5    44   43  739  pipeline
web-hn            483  56.7  16.7  54.1   75.5  14.3   12.0    20    6     32   83.4     9   55  736  vsync
web-wiki          493  55.7  16.7  50.1   83.9  15.0   10.0    24    9     38  100.1    11   59  735  vsync
web-apple         208  40.9  16.7 133.0  213.4   5.5    4.6    55   12     96  216.7    19   60  739  CPU render server
web-heavy         446  38.0  16.7  77.2 1027.0   2.2    0.7   111   20    253 1384.2    22   43  722  CPU com.apple.WebKit.WebContent:0x211df4ea0
home-r2           488  49.0  16.7  50.1  100.8  13.0    8.6    94    6    108  116.8    14   53  737  pipeline
settings-r2       558  56.2  16.7  41.7   66.7  18.0   15.0    30    6     38   66.7    17   51  737  pipeline
calendar-r2       592  57.2  16.7  33.4   66.7  21.2   15.0    23    3     28   66.7    97   54  737  vsync
maps-r2           243  45.0  16.7  88.1  151.1   8.2    6.0    49   14     79  166.8    63   48  738  pipeline
web-hn-r2         492  57.4  16.7  33.4   92.0  15.8   10.0    13    4     22  100.1    13   51  736  vsync
web-wiki-r2       457  59.1  16.7  33.3   60.9  23.1   12.0     7    1     10   83.4   101   45  736  vsync
web-apple-r2      509  57.7  16.7  33.4   66.7  18.9   15.0    19    5     26   66.7    14   45  736  vsync
web-heavy-r2      519  52.4  16.7  50.0   66.7  18.0   15.0    64    9     75   66.7    14   43  735  CPU com.apple.WebKit.WebContent:0x211df4ea0
ALL              7757  52.3  16.7  50.1  133.4   9.2    2.7   740  139   1151 1384.2    19   51  736  (every scene's motion frames pooled)
home: 8 windows, 9.7 s of motion, 95% of frames on time; source latch
```

Run-to-run spread (base2: the two passes; (max - min) / mean):

```
home           fps 55.9/49.0 (13%) | p99 50.0/50.1 (0%) | low1 12.8/13.0 (1%) | low01 10.0/8.6 (15%) | over16 28/94 (108%) | over33 7/6 (15%) | max 100/117 (15%) | gpu_busy 60/53 (13%)
settings       fps 58.4/56.2 (4%) | p99 33.4/41.7 (22%) | low1 21.2/18.0 (16%) | low01 15.0/15.0 (0%) | over16 14/30 (73%) | over33 4/6 (40%) | max 67/67 (0%) | gpu_busy 52/51 (0%)
calendar       fps 57.4/57.2 (0%) | p99 33.4/33.4 (0%) | low1 20.0/21.2 (6%) | low01 15.0/15.0 (0%) | over16 21/23 (9%) | over33 5/3 (50%) | max 67/67 (0%) | gpu_busy 55/54 (2%)
maps           fps 40.3/45.0 (11%) | p99 114.7/88.1 (26%) | low1 8.2/8.2 (0%) | low01 7.5/6.0 (22%) | over16 71/49 (37%) | over33 13/14 (7%) | max 133/167 (22%) | gpu_busy 43/48 (11%)
web-hn         fps 56.7/57.4 (1%) | p99 54.1/33.4 (47%) | low1 14.3/15.8 (10%) | low01 12.0/10.0 (18%) | over16 20/13 (42%) | over33 6/4 (40%) | max 83/100 (18%) | gpu_busy 55/51 (9%)
web-wiki       fps 55.7/59.1 (6%) | p99 50.1/33.3 (40%) | low1 15.0/23.1 (42%) | low01 10.0/12.0 (18%) | over16 24/7 (110%) | over33 9/1 (160%) | max 100/83 (18%) | gpu_busy 59/45 (26%)
web-apple      fps 40.9/57.7 (34%) | p99 133.0/33.4 (120%) | low1 5.5/18.9 (111%) | low01 4.6/15.0 (106%) | over16 55/19 (97%) | over33 12/5 (82%) | max 217/67 (106%) | gpu_busy 60/45 (28%)
web-heavy      fps 38.0/52.4 (32%) | p99 77.2/50.0 (43%) | low1 2.2/18.0 (156%) | low01 0.7/15.0 (182%) | over16 111/64 (54%) | over33 20/9 (76%) | max 1384/67 (182%) | gpu_busy 43/43 (0%)
```

- **Noise.** fps agrees within 0-6% on Settings, Calendar and warm web pages, 11-13% on home and Maps. The tails do not: one scene
  has 200-600 frames, so its 0.1% low is its worst frame or two, and 1% low its worst 2-6. A web page's first visit (decoding,
  layout, network) is much worse than the second (web-apple 40.9 vs 57.7 fps, web-heavy 0.7 vs 15.0 0.1% low). Judge a change on
  the pooled ALL row and on fps / >16.7 / >33 counts per scene, with ROUNDS >= 2 interleaved (A/B mode), and compare warm passes
  with warm passes. The App Library was not reached once in four tries (home-page1.sh --applib's match), so a scene can be missing.
- **What the panel shows.** p50 is one vsync everywhere: steady frames are on time. The losses are frames of 2-3 vsyncs (33-50 ms)
  in every scene, and longer stalls (67-217 ms) in the App Library, Maps and web pages on first visit. Pooled over the run (7757
  frames): 52.3 fps, 1% low 9.2, 0.1% low 2.7 (one 1.4 s stall on web-heavy's first pass), 740 frames over one vsync.
- **GPU** runs at the top level (L7, ~737 MHz) during every motion window; the governor ramps at once. Busy 43-60% over motion,
  peaks of 100% in 20 ms samples. The SP is mostly idle while busy (ALU 17-37% of SP busy, few TP stalls): the work is fill and
  bandwidth (full-screen sysmem passes and the presenter's copy), not shader math. Thermal: 63-72 C, no cap.
- **CPU:** no core is saturated (the A75s at 27-33%, the A55s 2-12%). The render server's busiest thread
  (com.apple.coreanimation.display.primary) runs in 29-36% of samples. Only web-heavy has a saturated thread: WebKit's WebContent main
  thread runs 98% of the time (layout, scripts): that scene is bound by the page, not by graphics.
- **The present path** is the largest per-frame cost besides CA's own GPU work. Frame trace medians (ms): CA GPU 5-10, commit
  (translation and submission on the encode worker) 6-12, present (swap offer until the presenter's copy returned) 8-18. In
  steady motion on home: offer -> copy GPU start 7.7 ms (the copy waits for the same frame's CA work still on the GPU), copy 3.6 ms,
  retire +1 ms, latch at the next vsync. The display hands the presenter one swap at a time, so the next offer waits for that retire;
  per frame the GPU does CA (~9.5 ms) plus the copy (3.6 ms) back to back, and frames whose CA work runs a little long miss the vsync.
- **Stability:** no backboardd crash, no swap_wait timeout, no GPU fault, no reclaimed hand-off over both runs. App crashes in the log
  are webbookmarksd, Maps, MobileMail, Safari's History service and (once, base1) com.apple.WebKit.GPU.
- **Correctness:** base1's rest screenshots were the first references, and base2 against them showed two problems with the
  references themselves. First, the rest points depended on where the drags ended (Settings' list stopped at another offset), so
  every rest point now taps the status bar first (scroll to top). Second, base1's home screen had the dock's Phone icon missing (a
  smear where the icon belongs): a real glitch, seen once (base2's home has the icon). The DIFF test now counts strong differences
  (over 96: an icon, text or a block missing or moved), not a blur's rounding (the App Library backdrop differs by up to 95 between
  runs). Seen by eye: Maps shows no map tiles (only the beige base), and theverge.com shows a black rectangle where a media element
  is. Both still need to be checked as data or rendering.

## Phase 2 log (burn-down)

Method: every change against a baseline from the same branch, judged on the pooled ALL row and the per-scene >16.7 / >33 counts
(a scene's 0.1% low is its one or two worst frames). Boot-to-boot noise is large, so a change is A/B'd in one boot where it can be
toggled (gpu-env, `gfx-suite.sh ab`), else across boots with REPEAT=2.

Baseline b0: ios16-port dd2cc5b (stack 43c46a6), full suite REPEAT=2, warm-up and settle check: ALL 8458 frames, 53.2 fps, 1% low 13.0,
0.1% low 7.1, 818 frames over one vsync, 125 over two.

### Where a steady frame's time goes (frame trace, home, steady motion)

    CA frame N:   created ... commit (encode 2 ms) ... committed (+7.8 ms: the encode worker translates and submits) ... GPU 9.5 ms
    offer N  -> presenter commits its copy at once; the copy waits on the GPU for frame N's own work (in order), runs 3.6 ms
             -> waitUntilCompleted returns -> (was: 1.4-6.5 ms more, see below) -> COMPLETE_SWAP (1 ms) -> retire -> flip latches at the
                next frame start
The display hands the presenter one swap at a time, so the next offer waits for this retire. The render server's loop (prep ~7 ms,
encode 2, the commit wait for translation ~7.8) is ~16.8 ms, and the retire lands ~2 ms before a vsync: little slack, at a phase that
drifts (retire-to-vsync spread over 0-16 ms), so a little jitter turns into a missed vsync.

### Measured

| change | how measured | result |
| --- | --- | --- |
| scanout presenter: real-time thread (NGPU_PRESENT_RT, stack b08101a) | gpu-env A/B, one boot | no change (its post-copy lag was not preemption) |
| presenter: release the flipped frame's command buffer later | frame trace | no change in the lag |
| frontend: a finished command buffer's `status`/`error`/wait do not drain the encode worker (stack 484b83f) | Q lines, before/after boots | presenter copy-complete -> swap-complete 1.4-6.5 ms median (p90 to 9.8) -> 0.01 ms; pooled >16.7 305 -> 278 on 5 scenes x2 (small) |
| GMEM (TU_DEBUG=nocb) vs sysmem | gpu-env A/B | mixed, within noise; CA GPU time 9.6 -> 8.9 ms home, 10.5 -> 11.9 App Library |
| NGPU_SCHEDULED_EARLY=1 | gpu-env A/B | worse: home >16.7 42 -> 73, Settings 24 -> 64; kept off |
| CA_FORCE_TIMER=1 (CADeviceUseVBL) | hw vsync counters | no vsync client registers either way; CA keeps its own display timer |
| 59.94 Hz IOMFBDisplayRefresh (fajita-refresh-us) | pending a same-build A/B | CA's swap period median 16.65 ms with or without it |
| job ring back-pressure (43c46a6's retry) | kernel counters per scene | 0 refused submissions in every scene (8 of 8 outstanding reached without a refusal): not a bottleneck |
| frame info to the render server (Presentation_time, Vbl_FrameTime per latched frame; d6763c9, fajita-frame-info) | same build, boot arg off/on (fi0/fi1), 5 scenes x2 | works (CA asks SupportedFrameInfo, registers the type-3 source; every latch delivered) but no measurable change: >16.7 and >33 within noise, no phase lock in the retire-to-frame-start histogram. Not merged |

### Background CPU (a CPU bottleneck beside the scenes)

gfx-suite's CPU windows show daemons busy during scenes, at random between passes, and they explain much of the noise:
healthappd 59-139% and healthd 22-59% (b0's applib-r2 at 37 fps against 48.6 on the first pass), coreduetd 91-94% through whole home
scenes on two boots, mobileassetd 63-98% and kbd 40-50% when the App Library first opens, mediaserverd 58-97% on www.apple.com (its
video, decoded on the CPU), CalendarWidgetExtension 25-72%, ReportCrash after an app crash, Spotlight 37-54% in Maps. The suite now
samples any background process over 40% after a scene (proc-sample.sh: thread stacks, symbolized against the shared cache).

### CA's display timer (static model, QuartzCore 20H392; to be measured with `--cawatch`)

Read with `tools/crash/dsc-disasm.py --syms --strings QuartzCore.framework/QuartzCore OFF N` (offsets image-relative; S = the
IOMFBServer, D = its IOMFBDisplay at S+0x60, T = the `_CADisplayTimer`).

- **The timer** is a Mach timer on its own port (`display_timer_callback` +0x36cc8, a CFMachPort callback on the render server's
  display thread). Each firing: now = mach_absolute_time; with D' = T+0xe0 (this firing's deadline) and I = T+0xd8 (the interval),
  if 0 <= now - D' <= I it re-arms at align(D' - slack) + I (`get_phase` against the timing reference, `mk_timer_arm_leeway`,
  leeway 0) **before** calling the client, and passes D' as the tick time. A firing more than one interval late (the previous
  callback ran > 2 I) re-phases from now (D' := now or now - I) and the skipped deadlines are gone: the LATE class.
- **The client is the renderer itself.** `IOMFBServer::timer_callback` (+0xccb24) aligns the phase (`_CADisplayTimerAlignPhase`
  with `IOMFBDisplay::timing_reference`) and tail-calls `IOMFBServer::vsync_callback(fb = NULL, frame#, T, I, S)` (+0x361ec), which
  calls `Server::render_for_time` synchronously on the same thread. So a frame's whole CPU side (prepare, encode, the commit wait
  for translation) runs on the timer thread; a frame over one interval makes the next callback late (still the next deadline), a
  frame over two loses ticks.
- **Gates in vsync_callback, in order:** S+0x2e0 := T (every tick); if S+0x2d8 (the vbl threshold) > T the tick is dropped
  (S+0x30d |= 1): THRESHOLD. Otherwise, with S+0x2b0 the next update time (a double in CA time; inf when nothing is due): if inf,
  the idle countdown S+0x308 drops by one and at 0 the timer stops (`set_vsync_disabled`); if the next update is later than T + I
  (the frame this tick would show), CA only collects (`Render::collect`): NOTDUE. Else S+0x2b0 := inf and render_for_time(T + I).
- **The threshold** (`IOMFBServer::update_vbl_threshold` +0x27d464, vtable +0x1c0) = t + max(vbl_delta, flag ? 4 frames : 0),
  vbl_delta = one panel period (`IOMFBServer::vbl_delta` +0x27c9e0, display vtable +0x2d0). render_for_time sets it at the end of
  a frame: on the timer path (arg 5 = 0) only when `CADisplayEnableAllQuantaMultiples`, to target - half a period (harmless); on the
  **immediate path** (arg 5 = 1, `IOMFBServer::immediate_render`'s block +0x27d52c, entered from `Server::context_changed` +0x4ba18
  when a client's commit asks for an immediate update) to **now + one period**: an immediate render just after a tick drops the next
  tick, the classic 2-tick gap with nothing slow anywhere.
- **can_update** (`IOMFBDisplay::can_update` +0xa1804 -> `can_update_` +0x3a944) walks CA's swap ring (D+0x358 head, +0x35c tail,
  +0x360 count, capped at 2 in flight; 0x30-byte entries from D+0x3a8, swap id at +0x3c0) and frees each entry whose swap
  `IOMobileFramebufferSwapWait` (or `swap_wait_timeout`, by the CADeviceUseVBL flag) reports done; when none is free the frame is
  refused and D+0x258 counts it: CANT. With our display handing the presenter one swap at a time and the retire ~2 ms before a
  vsync, a frame whose previous swap retires after the tick waits a whole period here.
- **The phase reference**: `IOMFBDisplay::timing_reference` (+0x3840c) reads CA's own timing shmem (D+0x4c0, made by
  `initialize_timings`: +0x28/+0x30 reference times, +0x38 period); zero means no reference and the timer free-runs at its own
  phase (the drifting retire-to-vsync phase seen in b0). It is fed only by `IOMFBDisplay::add_timing` (+0x2b933c), called only from
  `IOMFBServer::frame_info_callback` (+0x386d4), and only after `IOMFBDisplay::collect_frame_info(swap_id)` (+0x3b880) finds CA's
  record of that swap: an exact match on the swap id CA got from the swap (records with lower ids are discarded). d6763c9's frame
  info sent the family's completed generation as the id; if that is not CA's swap id every record misses and add_timing never
  runs, which would explain fi1's "works but no phase lock". To check with `--cawatch` (the `ref` fields) before any rework.
- **CADeviceUseVBL** (`___CADeviceUseVBL_block_invoke` +0xe90e4): CA_FORCE_TIMER (nonzero) or a ChipID in {0x7000, 0x7001, 0x8000,
  0x8001, 0x8002, 0x8003, 0x8004, 0x8010} sets the flag (0x8015, the iPhone X we present as, does not); it is read once, when
  IOMFBServer/EAGLServer and AppleDisplay are built, which is before the GPU plugin's gpu-env setenv: that is why CA_FORCE_TIMER
  in gpu-env had no effect. Experiment (approved, default off, not for landing without numbers): boot-arg `fajita-ca-force-timer=1`
  (patches/xnu/zzt-fajita-ca-force-timer.patch) gives backboardd alone CA_FORCE_TIMER=1 at exec. Its effects (static): S+0x30d bit
  1, D+0x220 bit 5, SwapWait instead of swap_wait_timeout in can_update_, and set_next_update's generic Server path when no IOMFB
  vsync source is registered (S+0x268).

**Measuring it:** `gfx-scene.py --cawatch /private/var/ngpu/sbin/fjsurface-SHA ...` records `fjsurface --watch-ca` (S, D, T and the
timing shmem polled every 200 us, found by S's vtable pointer) in place of the CPU windows; `tools/gpu/ca-timer.py SCENE.json`
classifies every tick (RENDER, CANT, THRESHOLD, NOTDUE, IDLE, LATE), lists each 2+ tick gap between renders with its ticks'
classes and the retire ring around it, says whether the phase reference was ever set and histograms the ticks' phase against the
panel's rd_ptr ring. Pending a free phone (the daemons agent's Data reset).

### Ready for the phone (after the Data reset: prime-done, REBASE, new settled b0 first)

| item | build / how | host evidence | on the phone |
| --- | --- | --- | --- |
| CA timer gates and phase reference | any build; `CAWATCH=1 gfx-suite.sh` (or gfx-scene.py --cawatch), `tools/gpu/ca-timer.py` | static model above | which gate makes each 2-tick gap; whether frame info ever sets the phase reference |
| CA_FORCE_TIMER (CADeviceUseVBL) experiment | gfx 33d12c3 kernel (.cache/fajita-out); boot-arg `fajita-ca-force-timer=1` vs off, same build | patch in the kernel (strings present) | A/B REPEAT=2 against the new b0; landing needs the lead's approval |
| metal2vulkan draw-cost (johnpanos/metal2vulkan draw-cost f35f75b1 = 1ff8f4b3 + bench fix) | stack f6ad4e9 (fork fajita-gfx-m2v; NativeMetal gfx-m2v 3d54a97: two bridge patches refreshed), payload in .cache/fajita-out-m2v (built rc 0 from board 33d12c3) | encode bench STEADY=1 (lavapipe, callgrind): 35333 -> 34357 instructions per draw (-2.8%), 19.5 -> 10.8 allocations per draw | frame-trace commit wait per draw, REPEAT=2 against b0 |
| frame info with CA's swap id | same 33d12c3 kernel: `fajita-frame-info=0` (ios16-port behaviour), `1` (default here), `2` | swap_wait_gated: generation and request +788 share numbering | fi_id_differs, CA's FrameInfo records in --cawatch, phase reference set or not |
| CA frame capture for the host replay | `ca-capture.sh [FRAMES [SKIP]]` (needs fajita-ssh=1) | - | pulls backboardd's command buffers over Wi-Fi, builds frames.json for command-replay |

### Frame info with CA's exact swap id (fajita-frame-info=2)

swap_wait_gated (the original family, 20H392) waits for a swap by comparing the waited id with the display's completed
generation (+5220) and the current request's own id (request +788, `ldr w8, [x8, #0x314]` at 0xfffffff004c114b4): the request's
word at +788 is the id SwapBegin gave CA, and the completed generation follows the same numbering. So mode 1 (the generation at
the latch) should mostly carry the right id already; mode 2 sends the id read from the request that completed last (read in
finishTransfer before completeCurrentSwap), qualified in build-native-legacy-display.py (swap_id=788, swap_id_checks), and counts
in native_legacy_frame_info[4] the frames where the two differ ([5]: the last id sent; gfx-scene records them as fi_*). If they
never differ, the missing phase lock has another cause: `--cawatch` now also reads CA's FrameInfo records (D+0x448..0x450, the
oldest's id) and D+0x9f2's flags (bit 35: finish_update keeps a record per swap only when set). Records that pile up = ids never
match; no records = CA keeps none, and frame info is ignored whatever its id. Arms by boot arg on one kernel: fajita-frame-info=0
(as ios16-port), 1, 2; fajita-ca-force-timer=0/1.

### Two swaps in flight (design, not coded)

Today the display hands the presenter one swap at a time: the next offer waits for this one's COMPLETE_SWAP and retire (in flip
mode the retire is at the DPU latch unless retireEarly), so swap N+1's copy is committed only after N's round trip, even though
CA's ring (`can_update_`, two entries) already has N+1 submitted. Accepting a second in-flight swap needs:
- **Presenter (NativeGOPPresenter):** the single `Swap swap` becomes a FIFO of two entries, each with offered/delivered, sequence,
  target, deliveredTask and deliveredAt. waitSwap delivers the oldest offered-undelivered entry; completeSwap matches by sequence and
  must complete in order (a later one finishing first is held). freeTarget must also exclude the targets of delivered-uncompleted
  entries. With three scan-out buffers (front, pending flip, queued) a second copy has a target only while no flip is pending: full
  pipelining wants a fourth buffer (10.1 MB at 1080x2340) or the second offer is refused when no target is free (then it behaves
  as today).
- **Display (NativeLegacyDisplay):** `gpuRequest` and the `transferred` latch become a two-entry in-flight list; kickSwapQueue offers
  the request behind the one in flight (the family's pending head stays N; N+1 is found through NATIVE_LEGACY_REQUEST_NEXT); a
  finishTransfer for N+1 that arrives before N's (a failed GPU copy of N falling back to the CPU) waits until N is completed, because
  activateOnlyPendingSwap only completes the pending head.
- **The frozen-screen fixes (11232f7, 8a71b82):** both rely on one latch. With two in flight: a client that died takes both requests
  (each finishTransfer then takes the why==1 path: drop, count, and kick only when nothing else is in flight); the presenter's
  reclaim must fail every entry delivered to the dead task, oldest first, so the display completes them in order; a reclaimed
  entry's target is freed with it. The "keep the latch set while completion can reenter" rule becomes "no new offer while a
  completion is running".
- **What it can buy:** only the gap between N's retire and N+1's offer (COMPLETE_SWAP ~1 ms, the kick and the presenter's wake); the
  copy still waits on the GPU for CA's own N+1 work (offer -> copy start 7.7 ms today is mostly that). Worth it only if the CAWATCH
  pass shows CANT ticks whose previous retire lands within a couple of ms after the tick. About 150-200 lines across two drivers with
  the wedge invariants above: not small, so not coded until the watcher says CANT is a real share of the gaps.
