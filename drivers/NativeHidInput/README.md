# NativeHidInput

HID input driver. Publishes keyboard, pointer and touch events to IOHIDFamily.

Current state: used by the sheng touch path.

Linked into XNU by `unixnu build kernel`.

fajita's copy of UniXNU's NativeHidInput (588c96f) for both fajita boards: the IOHIDFamily vtable slots and the IOHIDEvent
`_children` offset of the edge-tip wrapper come from `OriginalHidEventLayout.h`, which `scripts/build-native-legacy-display.py`
generates for the kernelcache the kernel is prelinked with (21H461 for the iPad board, 20H392 for the ios16 board), after checking
each slot and the field against the instructions of that kernelcache's candidate.

`native-hid-buttons=1` (fajita): report 2 of the keyboard collection carries Consumer Menu, Power, Volume Increment and Volume
Decrement (bits 0..3) instead of Menu alone, and the PMIC keys of `package/FajitaPmic.cpp` (`fajita-pmic-keys=1`,
`native_button_state` and `native_button_set_sink`) are sent through it on every change. `native_hid_button_stats` counts the
reports. Without the argument the descriptor and reports are unchanged.
