// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual firmware scanout transfer, not a compositor or hardware vblank ABI.
#pragma once
#include <stdint.h>
enum { NGP_BGRX8888=1,NGP_VERSION=2 };
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPPresent {uint64_t address,bytes;uint32_t width,height,stride,format;};
struct NGPTransfer {uint64_t sequence,bytes;};
// GPU hand-off (boot-arg sheng-fb-gpu=1): the display driver offers a finished frame instead of copying it. A privileged client waits for it
// (selector 2: the frame's pages are mapped read/write into the caller, at address), lets the GPU copy it into the scanout mapping (client
// memory type 1, writable) and then completes it (selector 3), which retires the swap. Nothing here copies pixels.
enum { NGP_METHOD_MODE=0,NGP_METHOD_PRESENT=1,NGP_METHOD_WAIT_SWAP=2,NGP_METHOD_COMPLETE_SWAP=3,NGP_MEMORY_SCANOUT_RW=1,NGP_MEMORY_SCANOUT_BACK_RW=2 };
// Double-buffered scan-out (fajita-fb-flip=1, version 2): NGPSwap.target says which buffer the GPU writes (0: the scanout, memory type 1; 1: the
// back buffer, memory type 2), never the one on screen; flags bit 0 says the frame is flipped, so the copy must be complete before the swap is
// completed (the display hardware takes the buffer at its next frame boundary). NGPComplete.reserved echoes the target written.
struct NGPWait {uint64_t timeout_ms;};
// NGPSwap.reserved: nonzero when the frame's mapping is kept across swaps (fajita-gop-mapcache): the same value means the same mapping at the same
// address over the same pages, so the client may keep its import of it; a value it no longer sees was released. 0: mapped for this swap only.
struct NGPSwap {uint64_t sequence,address,bytes,stride;uint32_t width,height,format,reserved,target,flags;};
struct NGPComplete {uint64_t sequence;uint32_t status,reserved;};
static_assert(sizeof(NGPWait)==8 && sizeof(NGPSwap)==56 && sizeof(NGPComplete)==16,"GOP hand-off ABI");
static_assert(sizeof(NGPMode)==40 && sizeof(NGPPresent)==32 && sizeof(NGPTransfer)==16,"GOP transfer ABI");
