#!/bin/bash
# Build and run the NativeDwc3 host unit tests (register decode, SMMU plan, fjdbg
# framing + command layer), then drive the host tool scripts/fajita-usbdbg.py against
# UsbDbgSim (the device-side fjdbg code over stdin/stdout).
# Pure host build, like drivers/NativeUfs/tests: c++ -std=c++17, no kernel headers.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-c++}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for t in Dwc3Decode Dwc3Smmu UsbDbgProto AcmEnum; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -O2 -o "$TMP/$t" "$t.cpp"
  "$TMP/$t"
done
"$CXX" -std=c++17 -Wall -Wextra -Werror -O2 -o "$TMP/UsbDbgSim" UsbDbgSim.cpp
python3 -I ../../../scripts/fajita-usbdbg.py selftest --sim "$TMP/UsbDbgSim"
echo "NativeDwc3 host tests: PASS"
