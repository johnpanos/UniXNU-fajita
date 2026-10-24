// SPDX-License-Identifier: MIT
// The iPhoneOS SDK ships IOKit.framework without headers: the few user-client calls the GPU tools make, with the real IOReturn and
// map-option values (they link against the SDK's IOKit.tbd).
#pragma once
#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef uint32_t IOOptionBits;
typedef kern_return_t IOReturn;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFDictionary *CFDictionaryRef;
#define kIOMainPortDefault ((mach_port_t)0)
#define kIOReturnSuccess KERN_SUCCESS
enum {kIOMapAnywhere = 0x00000001, kIOMapWriteCombineCache = 0x00000400};
CFMutableDictionaryRef IOServiceMatching(const char *name);
CFMutableDictionaryRef IOServiceNameMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *inputStruct, size_t inputStructCnt,
                                        void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask, mach_vm_address_t *atAddress,
                                   mach_vm_size_t *ofSize, IOOptionBits options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask, mach_vm_address_t atAddress);
#ifdef __cplusplus
}
#endif
