# Management log: goals, agents, decisions

The lead's running record of what the user asked for, which agents work on what, and the decisions made along the way, so a
later session (or another person) can pick up without the conversation. Newest first. Each agent's technical detail lives in its
own NOTES file; this log links to them.

## How the work is run
- 1-2 agents at a time, each in its own worktree (AGENTS.md: "One tree"), sharing the phone through the lock.
- Agents land each proven piece on ios16-port as soon as it is proven (full build first), not in batches.
- The lead checks every ~10 minutes: phone queue, bg.sh jobs, stuck waits, unmerged work. It verifies landings (origin, fork
  branches of pins) and fixes tooling gaps itself.
- A safety-classifier stop ends that piece (AGENTS.md); it is recorded and taken to the user, never reworded.

## 2026-10-27: paused; the phone boots iOS by itself (user request)
- The user asked to pause everything and have the phone default-boot iOS with the latest build. Both agents paused (drivers:
  868ffd3 on its branch; sensors: PAS/FastRPC/SSC/ext4 work on its branch, nothing unproven pushed).
- Installed a stay-up build of ios16-port 23fe098 (stack 7fa0794, GPU plugin rebuilt; Wi-Fi, iOS networking, wifid, SSH on)
  with scripts/fajita/install-default.sh: payload to system_a, boot image to boot_b, slot b active. It powers on into iOS and
  joins KittyCorner about 47 s after boot.
- Consequences: a reset boots iOS again, not fastboot, so unattended test tools need `install-default.sh --remove` first.
  Each power-on spends a slot retry (7); rerun the install or `fastboot set_active b` before they run out.

## 2026-10-26 (evening): goal "write as many high quality drivers for the fajita for iOS, easiest first"
- Drivers agent started (worktree ../UniXNU-fajita-drivers): inventory of ordinary peripherals vs what is driven, ranked by
  effort and value, in NOTES-drivers.md, then one driver at a time behind a flag, proven, then default and landed. Security and
  authentication hardware and cellular telephony are out of scope.
- Ranking landed (db1b095, NOTES-drivers.md), in work order: brightness to iOS (finish), alert slider to iOS (finish),
  thermal (CPU throttling, temperatures to iOS), hall sensor, vibration to iOS, flashlight, motion/light sensors (behind
  the sensor DSP), audio, Bluetooth, GNSS/video decode/cameras. Brightness and slider runs are under way.
- Landed: brightness (b0be604; brightness-check.sh proves the Control Center slider drives the panel). Next: alert slider,
  thermal (FajitaThermal.cpp with a host-tested throttling policy) in progress.
- Landed: thermal (a274b02): the CPU thermal governor is on by default; iOS temperature services behind fajita-thermal=3
  (experimental). The alert slider hit 7 phone runs without landing; the agent was told to land a fix or record it as open.
- The graphics smear hunt overran its 20-minute bound to 98 minutes and two stop messages; the lead killed the job to free the
  phone. Lesson: give a long hunt a hard --max in bg.sh at launch, not just a bound in a message.
- Graphics wrapped up (ff60ac7, NOTES-gfx.md): landed the bounds check (abnd, 55dfbd8) as a correctness fix; devc stays
  (a small win). Not landed: frame info, CA_FORCE_TIMER, the m2v draw-cost translator, the Mesa push-const picks, the
  100 ms fence wait. Open: render-server CPU per frame, late client commits, the GMEM arm, the App Library keyboard flake.
- Drivers: vibration needs an audio plug-in ("Actuator" for mediaserverd) loaded from /Library/Audio/Plug-Ins/HAL. The user
  approved (2026-10-27) the read-only boot-time overlay for it and following HackiOS's plug-in design (own code). The
  flashlight needs the camera driver's torch path, so it goes last. Slider: MobileGestaltHelper rebuilds the cache at boot;
  a uid-501 query test is next.
- The phone wedged in fastboot at 00:10 (no command answers; usbreset and reboot-bootloader failed). It needs a hardware
  reset (Power + Volume Up); the user was asked. The user reset it; it answers again.
- Vibration built (e2f2e82, fajita-haptics=3, default off): mediaserverd loads our Actuator plug-in through the overlay, but no
  I/O starts. Both vibration and the slider are gated by MobileGestalt's stale cache (ringer-switch and closed-loop haptics
  answer false; computed live as mobile they are true). Decision: use iOS's own rebuild request (sentinel, as mobile), not a
  direct rebuild. First, though: the clock read about 11 h behind after the hard reset, so the RTC path is checked first
  (a wrong clock is what made coreduetd spin).
- Second agent started: sensors (worktree ../UniXNU-fajita-sensors, NOTES-sensors.md). The SLPI sensor DSP boot reuses the
  Wi-Fi remoteproc/QRTR code, then ambient light, proximity and accelerometer samples, then iOS integration (auto-brightness,
  proximity screen-off, rotation). Phased, each phase landed behind a flag.
- Landed: the RTC fix (459437d: a hard reset no longer starts the clock days in the past); the alert slider is iOS's ringer
  switch by default, proven (ef5b5d3; MobileGestalt rebuilt through iOS's own request). Sensors plan landed (705a8e2:
  SLPI PAS boot, FastRPC registry, SSC protocol); its PAS boot code is in progress. Vibration is next for the drivers agent.
- The bootloader wedged a second time (a session killed during its fastboot step), and the user was asked for another hard
  reset. Workflow fix by the lead: boot.sh and cycle.sh run every fastboot command in its own session (fb(): setsid -w,
  bounded by a timeout), so a kill of the job's process group (bg.sh kill, --max) can't cut a fastboot exchange in half.
- The graphics agent finishes its in-flight verdicts (devc keep/revert, the abnd smear fix) and wraps up NOTES-gfx.md.

## 2026-10-26: goal "optimize graphics for correctness and speed" (NOTES-gfx.md)
- Built the gfx suite (websites and apps, per-frame timing, 0.1% lows, bottleneck class, screenshots, stability).
- Landed: the suite and its tools; the backboardd crash fix (lost device on a full job ring); the frozen-screen fixes; the
  plugin cache path on fresh Data; the device-property cache (upstream 31867dc; verdict pending on alternating boots); the
  16384-object cap (upstream 25abaa4); the refused render encoder (upstream ce5e19e).
- Found: dropped frames come mostly from render-server CPU per frame (178 of 302 gaps) and late client commits (90). An
  intermittent texture smear was caught in a layer's own contents; the padded-row tile-store bounds check (abnd) is the fix
  candidate.
- Rejected after measuring: frame info, forcing CA's timer, two swaps in flight.
- Upstream survey of all repos (Mesa, metal2vulkan, NativeMetal, NativeAdreno, the stack, Apple sources): ranked picks are in
  the agent reports; Mesa correctness fixes are built, not landed.

## 2026-10-26: background daemons (NOTES-daemons.md)
- coreduetd's 94% spin was the boot clock starting at 1970 (fixed: RTC calendar), then one Data reset (user-approved).
- Sandbox stand-in returned garbage for container-less processes (fixed); en0 now exists from boot so configd creates its own
  network service on fresh Data.

## 2026-10-26: Wi-Fi (NOTES-wifi.md)
- Done: radio, WPA3-SAE, iOS's own networking, Apple's wifid running, translating SMMU bank, 5 GHz preference (KittyCorner's
  2.4 GHz BSS band-steers the phone).
- Open: Settings > Wi-Fi (wifid seeing the interface) is blocked by a safety classifier, on both the reverse-engineering and
  the open-source AirportItlwm routes. It needs the user, or a /feedback report.
- SSH dev mode over Wi-Fi works (NOTES-devmode.md); autoboot is blocked by a safety classifier.
