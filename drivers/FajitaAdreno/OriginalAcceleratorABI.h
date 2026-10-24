// SPDX-License-Identifier: MIT
#pragma once
// Copy of drivers/NativeMetal/OriginalAcceleratorABI.h (UniXNU NativeMetal) for the fajita kernel build, which has no NativeMetal
// module; build-fajita-gpu.py makes NativeAdrenoAccelerator.cpp's "../NativeMetal/" include find it. 20H392 holds the same facts
// (scripts/audit-fajita-accelerator-base.py).
// Exact supplied iOS17 base, independently audited against its complete
// original vtable and protected constructor/destructor bodies. No extra fields
// or operational overrides beyond IOService. This declares a real ancestry;
// no original class, method, vtable or metaclass is replaced.
#include <IOKit/IOService.h>
class IOAcceleratorES : public IOService {
 OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,"Qualified original accelerator base extent");
