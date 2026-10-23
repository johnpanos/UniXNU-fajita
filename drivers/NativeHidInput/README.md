# NativeHidInput

HID input driver. Publishes keyboard, pointer and touch events to IOHIDFamily.

Current state: used by the sheng touch path.

Linked into XNU by `unixnu build kernel`.

fajita's copy of UniXNU's NativeHidInput (588c96f) for both fajita boards: the IOHIDFamily vtable slots and the IOHIDEvent
`_children` offset of the edge-tip wrapper come from `OriginalHidEventLayout.h`, which `scripts/build-native-legacy-display.py`
generates for the kernelcache the kernel is prelinked with (21H461 for the iPad board, 20H392 for the ios16 board), after checking
each slot and the field against the instructions of that kernelcache's candidate.
