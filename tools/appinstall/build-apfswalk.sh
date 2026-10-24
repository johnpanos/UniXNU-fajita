#!/bin/bash
# Needs apfs-fuse (headers + build/*.a); APFS=/path/to/apfs-fuse
A=${APFS:-/home/john/ghidra-mcp/apfs-fuse}
g++ -std=c++17 -O1 -I"$A" -I"$A/3rdparty/lzfse/src" "$(dirname "$0")/apfswalk.cpp" -o "${1:-apfswalk}" "$A/build/libapfs.a" "$A/build/libcrypto.a" "$A/build/liblzfse.a" -lz -lbz2 -lpthread 2>&1 | grep -E 'error|undefined' | head
