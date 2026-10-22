# fajita iOS 16 board

The iOS 16.7.16 (iPhone10,6, 20H392) boot option of the OnePlus 6T. See ../NOTES-ios16.md for the route and the design.

| Item | State (2026-10-22) |
| --- | --- |
| IPSW members | fetched with range requests, sha256 pinned, SHA-384 matches BuildManifest.plist |
| xnu-8796.141.3 patch series | all patches apply (ios16-stack: patches/xnu-8796) |
| iig outputs | Xcode 15 (DriverKit-286) outputs of the unchanged 8796 .iig files, from the ios-sweet CI artifact, reviewed against the 21H461 set; to be replaced by a macos-14 run of the macOS build workflow |
| Candidate kernel | fajita_ios16_candidate_defconfig builds on Linux, links and prelinks against 20H392 (all 31 roots) |
| Full kernel | stops at build-fajita-storage.py: the storage audit, verified-root and PMGR builders still carry 21H461 values |
| Userdata image | built: version 2 manifest at 16 GiB, sparse image verified against the raw image |
| NativeUfs root selection | native-ufs-root-base, host tests pass |
| Payload with two entries | payload.py supports [[payload.include]]; not built (needs the full kernel) |
| Hardware | not tested |
