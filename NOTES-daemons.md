# Background daemons that burned CPU (coreduetd, healthappd/healthd, mobileassetd/kbd)

## Result

| daemon | before (old Data, 1970 clock) | after (clock fix + fresh Data, 2026-10-26) |
| --- | --- | --- |
| coreduetd | 91-94% for whole runs on Wi-Fi boots; 94.8% median over 34 min idle on a fixed-clock Wi-Fi boot (old Data) | 0.0% median on 3 boots (reset boot, no Wi-Fi; resumed, no Wi-Fi; Wi-Fi), never above 2% after the first minute |
| healthappd / healthd | 59-139% / 22-59% in gfx scenes; 120% / 15% at once when the clock jumped 1970 -> 2026 | healthappd one 10 s window at 8.2% in 6 rounds (Wi-Fi boot), healthd only in the first minute after boot |
| assetsd, searchd, cloudd, CacheDelete | 64-75% each right after the clock jump (dasd's maintenance activities, overdue by 56 years) | not seen at idle |
| mobileassetd / kbd | 63-98% / 40-50% when the App Library first opens (gfx suite) | not re-measured in the suite here (the graphics agent's baseline will show it); 0% at idle |

Idle rounds: `scripts/fajita/test/idle-daemons.sh` (10 s CPU windows, 120-180 s apart). Logs: `$FAJITA_WORK/daemons/{wifi2-prune,reset0,proofB,proofC-wifi}.txt`.

## Cause: the clock started at 1970 on every boot

coreduetd's busy thread served dasd: `_DKKnowledgeStorage executeQuery` -> `_DKPredictionQuery` -> `_DKBiomeQuery executeBiomeQueryError` ->
`_publisherForStreams` / `BMStreamDatastoreBase newEnumeratorFromStartTime:...` -> `BMStoreEnumeratorBase advanceBookmarkToStartTime`/`nextEvent` ->
`BMFrameStore enumerateWithOptions`, building enumerator after enumerator inside one request. The streams it held open (`fjbundle --fds coreduetd`)
were `restricted/_DKEvent.Dasd.Batterytemperature/local/0`, `_DKEvent.Dasd.WidgetView/local/...` and `public/AppIntent/local/...`.
Biome stores are time-ordered, and these were not:

- The calendar started at the Unix epoch on every boot (`fjbundle --find` printed `now time() 67` at 67 s uptime). The kernel is built in the
  BCM2837 configuration, where `IOKitInitializeTime` does not wait for the IORTC resource, and no platform expert forwarded the time of day to
  FajitaRtc: the base `IOPlatformExpert::getGMTTimeOfDay` answers 0 (ArmDTPlatform does not override it, though FajitaRtc's header assumed it
  did). FajitaRtc was also off by default (`fajita-rtc=0`). Its offset record (seq 1, written by the RTC work) was already correct to 6 s.
- On boots without Wi-Fi the clock stayed in 1970; on Wi-Fi boots (the graphics suite's) timed's network fix moved it mid-boot to 2026.
- A Biome segment created in 1970 is named `0` and every later boot appended to it. `tools/biome/segb.py` on pulled segments: 1970 and 2026
  frames interleaved, 11-88 steps back in time per segment; `Batterytemperature/0`: 222 frames, 79 steps back (126 frames of 1970, then a
  1970 frame or two after each 2026 run: one per boot before its network fix). 24 of the 28 stream segments were `0` segments.
- coreduetd spun only on Wi-Fi boots (the widget/prediction queries dasd makes with a network), and even when the clock was right all boot:
  the stored 1970 frames are enough. The jump itself also costs: setting the clock 1970 -> now on a quiet boot (`fjbundle --settime`) set off
  healthappd at 120% and healthd at 15% at once, then assetsd 75%, searchd 64%, CacheDeleteAppContainerCaches, containermanagerd, cloudd.

## Fix

1. The clock (ff94bbe, on ios16-port):
   - `patches/xnu/zzu-fajita-rtc-calendar.patch`: `IOKitInitializeTime` waits for IORTC when the device tree has `/arm-io/rtc` (FajitaRtc
     publishes it ~0.3 s after the calendar's start, a second before launchd); `IOPlatformExpert::get/setUTCTimeOfDay` ask the IORTC
     resource's `getGMTTimeOfDay` / `setGMTTimeOfDay` platform functions (Apple's ARM RTC interface).
   - `ios16/board.toml` board_args `fajita-rtc=2`: wall time = PM8998 RTC counter + the offset record in the userdata reserve; a settimeofday
     (timed, or `fjbundle --settime`) that moves it by 2 s or more rewrites the record. Re-anchor from the host with
     `session.sh run /private/var/ngpu/sbin/fjbundle-<hash> --settime $(date +%s)` (`FajitaRtc: time set ... record written` in the USB log).
2. The stored frames: Biome's own pruning does not remove them. 34 minutes idle and plugged in on a correct clock left all 226 frames of
   `Batterytemperature/0` in state 1. biomed's `com.apple.biome.prune-expired-events` is daily maintenance, and segment pruning
   (`BMSegmentManager pruneSegmentsToMaxAge:`) works from segment names and keeps each stream's newest segment, which for most streams is the
   mixed `0` one. So the Data volume was started over the designed way, once, after the clock fix landed: a boot with `dreset=1`
   (`sheng-data-reset=1`: NativeRamBlock drops the persisted Data overlay, `native_data_persist_state` 3 = fresh), and Apple's first-boot
   flow ran with a correct clock. Nothing on Data was edited by hand.

Any kernel without ff94bbe still writes 1970 frames into Biome on every boot (one landed from another agent's boot between my sessions).
Merge ios16-port before booting.

## The Data reset (2026-10-26 13:48, session-134756)

- DataMigrator reinstalled all 32 staged apps in about 4 minutes (32 bundle containers, 0 staged left); the home screen shows them; Setup
  `SetupDone` True, `setupMigratorVersion` 12.
- `DMLastMigrationResults` success **False** (it was True on the 24 Oct Data, before the App cryptex): Safari's migration plugin aborted
  3 times (`com.apple.migrationpluginwrapper`, sig6), see "Safari's container" below. DataMigrator also wrote 5 `stacks+com.apple.datamigrator`
  hang reports and one migrationpluginwrapper cpu_resource report.
- Crashes that predate the reset (also in the old-Data boots 102717-125251): webbookmarksd (same Safari cause), AegirPoster (sig6 from a
  FrontBoardServices serial-queue block; not investigated), MailWidgetExtension.
- Gone with the old Data, as expected: `/private/var/ngpu/wifi.psk` (wifi-test.sh pushes it every run; user-boot.sh needs a saved copy:
  run wifi-test.sh once), `/private/var/ssh` (dropbear host keys are regenerated: the SSH host key changes, NOTES-devmode.md),
  `/private/var/ngpu/gpu-env` (the default), the backboardd m2v shader cache (rebuilt on the first boot: backboardd ~98% for a few minutes).

## Safari's container (webbookmarksd, com.apple.Safari.History, Safari's migration plugin)

All three abort in SafariCore `-[NSFileManager(SafariNSFileManagerExtras) _safari_getMobileSafariContainerDirectoryURL:isContainerized:]`,
with `-[NSURL initFileURLWithPath:isDirectory:]: nil string parameter` (kernel `ABORT webbookmarksd reason ...` line). The method does
`if (sandbox_container_path_for_pid(getpid(), buf, 1024) == 0) url = [NSURL fileURLWithPath:@(buf) isDirectory:YES]` and otherwise asks
LaunchServices (`LSApplicationRecord initWithBundleIdentifier:@"com.apple.mobilesafari" ...`.dataContainerURL). LaunchServices is fine
(`fjbundle --lsrecord --mobile com.apple.mobilesafari`: /Applications/MobileSafari.app, data container
/private/var/mobile/Containers/Data/Application/<uuid>, System, installed). The Sandbox stand-in (stack patch
zzw-hackios-security-standins.patch, `minisandbox_container_path`) answered request 4 with success and an untouched buffer for a process
without a container, where the real Sandbox fails it. Fix (being tested): `ios16/patches/xnu-8796/zzw-hackios-z-sandbox-no-container.patch`,
ENOENT for a process without CFFIXED_USER_HOME (extensions and apps, which have it, are unchanged). Proof wanted: no webbookmarksd /
Safari.History ABORT lines, and `DMLastMigrationResults` success True on a fresh Data.

## Also found

- backboardd climbing from 27% to 90% on an idle lock screen (13:09-13:15): the phone's persistent `/private/var/ngpu/gpu-env` held
  `NGPU_CPU_PROFILE=backboardd`, left from a profiling session; the busy thread was the plugin's `cpuprof::sampler` (thread_suspend, symbol
  map, std::to_string). The plain gpu-env was pushed back and backboardd restarted at 13:15:30; idle numbers before that had the profiler on.
- A boot whose phone was left in iOS with no hub (the session's autoreset had not fired) fails `session.sh start` with "no fastboot at
  start"; `to-fastboot.sh` resets it over the debug gadget.

## How to rerun the checks

    scripts/fajita/build-usbdbg-agent.sh $FAJITA_OUT/tools/fjdbg-agent   # fjbundle, fjsurface, ngpu-metal into $FAJITA_OUT/tools
    scripts/fajita/test/session.sh start                                 # or a Wi-Fi held session (wifi-test.sh, WIFI_HOLD=1)
    scripts/fajita/test/bg.sh start idle1 --max 1500 scripts/fajita/test/idle-daemons.sh idle1 6 120   # CPU, clock, a Biome segment per round
    scripts/fajita/test/biome-survey.sh                                  # the clock, Biome/CoreDuet files, coreduetd's and dasd's open files
    PULL=/private/var/mobile/Library/Biome/streams/public/AppLaunch/local/0 scripts/fajita/test/biome-survey.sh
    python3 -I tools/biome/segb.py .cache/fajita-work/biome-survey/0     # frame times: steps back, from the future
    scripts/fajita/test/proc-sample.sh coreduetd 4 50 all                 # where a busy daemon spends its time (GPU plugin frames named)
    session.sh run /private/var/ngpu/sbin/fjbundle-<hash> --fds coreduetd   # what it has open; --lsrecord, --settime, --find as above

`grep FajitaRtc logs/NAME/usb-console.txt` shows the record, the wall time at bring-up and every set.
