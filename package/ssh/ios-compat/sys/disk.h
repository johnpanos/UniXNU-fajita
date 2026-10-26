/* fajita SSH dev mode: the two disk ioctls toybox uses (lib/portability.c, get_block_device_size); the iPhoneOS SDK ships no
 * <sys/disk.h>. Values as in xnu's bsd/sys/disk.h. */
#pragma once
#include <sys/ioctl.h>
#include <stdint.h>
#define DKIOCGETBLOCKSIZE _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
