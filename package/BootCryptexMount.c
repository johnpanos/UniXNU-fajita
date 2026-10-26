// SPDX-License-Identifier: MIT
// The App cryptex (Cryptex1,AppOS) of the iOS 16 board, mounted read-only where the OS volume's symlinks lead.
//
// The 20H392 OS volume has /System/Cryptexes/App -> ../../private/preboot/Cryptexes/App and /System/Cryptexes/OS ->
// ../../private/preboot/Cryptexes/OS. On a device the Preboot volume carries those directories and libignition grafts the cryptexes
// there; this board has no Preboot volume (/private/preboot is an empty directory of the sealed OS volume) and runs with ignition off,
// so launchd could not start the jobs whose programs live in the App cryptex (Safari's history, bookmarks, web inspector, push and
// password daemons), and lookups of their Mach services hung MobileSafari (NOTES-wifi.md section 9).
//
// fajita-appos=1 (board_args of ios16/board.toml; runs from sheng_boot_watchdog_policy, after the Data mount, before launchd):
//   1. the mount points live on Data: /private/var/db/fajita-preboot/Cryptexes/App (created once; persistent Data keeps them);
//   2. a read-only bindfs view of that directory is mounted over /private/preboot (the boot-time bindfs permission of the GPU addon);
//   3. the AppOS volume (APFS volume "AppCryptex", 9E10DDA7-..., the third root window of userdata, verified block by block against its
//      owner-signed policy by NativeUfs) is mounted read-only at /private/preboot/Cryptexes/App, with the SystemOS mount's payload;
//   4. the mount is checked: read-only, apfs, the selected device, still authorized, and a program of the cryptex reachable through
//      /System/Cryptexes/App.
// fajita-appos=2 also makes /private/preboot/Cryptexes/OS a symlink to / (SystemOS is the union at /, so /System/Cryptexes/OS/X is
// /X); adattributiond's program is /System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/Daemons/adattributiond. With =1 the
// link is removed if an earlier boot left it.
// fajita-appos-uuid=<UUID> names another volume. Never panics: a failure records its stage and errno (BootLog row C, the kernel log), and
// the jobs of a cryptex that is not there stay disabled (package/BootWatchdogPolicy.c), so their lookups fail at once instead of hanging.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>

extern int native_verified_block_volume_device(const char *, dev_t *, char *, size_t) __attribute__((weak_import));
extern int native_verified_block_root_authorized(dev_t) __attribute__((weak_import));
extern volatile int native_bindfs_boot_mount __attribute__((weak_import));

// Stage reached (8 = App mounted and checked, 9 = and the OS link made), the errno of the step that stopped it, the mode asked for,
// and the device mounted.
volatile int fajita_appos_stage = 0;
volatile int fajita_appos_status = -1;
volatile int fajita_appos_mode = 0;
volatile int fajita_appos_os_link = -1;
char fajita_appos_device[64];

static const char appos_volume[] = "9E10DDA7-90D9-4677-844A-E984BB1CCD0A";   // Cryptex1,AppOS of 20H392 (087-86715-047.dmg)
static const char data_dir[] = "/private/var/db/fajita-preboot";
static const char preboot[] = "/private/preboot";
static const char app_point[] = "/private/preboot/Cryptexes/App";
static const char os_link[] = "/private/var/db/fajita-preboot/Cryptexes/OS";
static const char probe_path[] = "/System/Cryptexes/App/usr/libexec/com.apple.Safari.History";

// The scratch page (one page of launchd's future address space): other boot steps use +0x200..+0x8ff; these offsets are free.
enum { kType = 0x900, kPath = 0x920, kSpec = 0x980, kData = 0xa00, kName = 0xc00, kLink = 0xd00 };

static int put(uint64_t scratch, unsigned offset, const char *text) {
 return copyout(text, scratch + offset, strlen(text) + 1);
}

static int make_dir(proc_t p, uint64_t scratch, const char *path) {
 int32_t result = 0;
 int error = put(scratch, kName, path);
 if (error) return error;
 struct mkdir_args md = {.path = scratch + kName, .mode = 0755};
 error = mkdir(p, &md, &result);
 return error == EEXIST ? 0 : error;
}

// =2: Cryptexes/OS -> /; =1: no link. Returns 0 or the errno.
static int os_link_set(proc_t p, uint64_t scratch, int want) {
 int32_t result = 0;
 int error = put(scratch, kName, os_link);
 if (error) return error;
 struct unlink_args rm = {.path = scratch + kName};
 error = unlink(p, &rm, &result);
 if (error && error != ENOENT) return error;
 if (!want) return 0;
 if ((error = put(scratch, kLink, "/"))) return error;
 struct symlink_args ln = {.path = scratch + kLink, .link = scratch + kName};
 return symlink(p, &ln, &result);
}

static int check_mount(const char *point, dev_t selected) {
 vnode_t root = NULLVP;
 int error = vnode_lookup(point, 0, &root, vfs_context_current());
 if (error) return error;
 mount_t mp = vnode_mount(root);
 vnode_t device = vfs_devvp(mp);
 dev_t actual = device ? vnode_specrdev(device) : NODEV;
 if (device) vnode_put(device);
 int ok = vnode_isvroot(root) && (vfs_flags(mp) & MNT_RDONLY) && actual == selected &&
  !strcmp(vfs_statfs(mp)->f_fstypename, "apfs") && native_verified_block_root_authorized(actual);
 vnode_put(root);
 return ok ? 0 : EAUTH;
}

// Returns 1 when the App cryptex is mounted and checked (and with mode 2 the OS link is in place), else 0.
int fajita_boot_cryptex_mount(void *process, uint64_t scratch) {
 proc_t p = process;
 int mode = 0, error = 0;
 int32_t result = 0;
 char volume[40];
 PE_parse_boot_argn("fajita-appos", &mode, sizeof(mode));
 fajita_appos_mode = mode;
 if (mode != 1 && mode != 2) return 0;
 strlcpy(volume, appos_volume, sizeof(volume));
 PE_parse_boot_argn("fajita-appos-uuid", volume, sizeof(volume));
 volume[sizeof(volume) - 1] = 0;
 fajita_appos_stage = 1;
 if (!native_verified_block_volume_device || !native_verified_block_root_authorized || !&native_bindfs_boot_mount) {
  error = ENOTSUP;
  goto done;
 }
 // 1. Mount points on Data.
 fajita_appos_stage = 2;
 if ((error = make_dir(p, scratch, data_dir)) || (error = make_dir(p, scratch, "/private/var/db/fajita-preboot/Cryptexes")) ||
     (error = make_dir(p, scratch, "/private/var/db/fajita-preboot/Cryptexes/App")))
  goto done;
 fajita_appos_os_link = os_link_set(p, scratch, mode == 2);
 // 2. Read-only bindfs view over /private/preboot.
 fajita_appos_stage = 3;
 if ((error = put(scratch, kType, "bindfs")) || (error = put(scratch, kPath, preboot)) || (error = put(scratch, kData, data_dir)))
  goto done;
 struct mount_args bind = {.type = scratch + kType, .path = scratch + kPath, .flags = MNT_RDONLY, .data = scratch + kData};
 native_bindfs_boot_mount = 1;
 error = mount(p, &bind, &result);
 native_bindfs_boot_mount = 0;
 if (error) goto done;
 fajita_appos_stage = 4;
 vnode_t point = NULLVP;
 if ((error = vnode_lookup(app_point, 0, &point, vfs_context_current()))) goto done;
 vnode_put(point);
 // 3. The verified AppOS volume, read-only, with the SystemOS mount's APFS payload (kern_exec.c native_owner_cache_prepare).
 fajita_appos_stage = 5;
 dev_t selected = NODEV;
 char device_path[64] = {0};
 if (!native_verified_block_volume_device(volume, &selected, device_path, sizeof(device_path))) {
  error = ENOENT;
  goto done;
 }
 strlcpy(fajita_appos_device, device_path, sizeof(fajita_appos_device));
 fajita_appos_stage = 6;
 uint8_t payload[0x150] = {0};
 user64_addr_t spec = scratch + kSpec;
 uint64_t flags = MNT_RDONLY;
 uint16_t access = 1;
 uint32_t anonymous = 99;
 memcpy(payload, &spec, sizeof(spec));
 memcpy(payload + 8, &flags, sizeof(flags));
 memcpy(payload + 16, &access, sizeof(access));
 memcpy(payload + 0x144, &anonymous, sizeof(anonymous));
 memcpy(payload + 0x148, &anonymous, sizeof(anonymous));
 if ((error = put(scratch, kType, "apfs")) || (error = put(scratch, kPath, app_point)) || (error = put(scratch, kSpec, device_path)) ||
     (error = copyout(payload, scratch + kData, sizeof(payload))))
  goto done;
 struct mount_args request = {.type = scratch + kType, .path = scratch + kPath, .flags = MNT_RDONLY, .data = scratch + kData};
 if ((error = mount(p, &request, &result))) goto done;
 // 4. The checks.
 fajita_appos_stage = 7;
 if ((error = check_mount(app_point, selected))) goto done;
 vnode_t program = NULLVP;
 if ((error = vnode_lookup(probe_path, 0, &program, vfs_context_current()))) goto done;
 vnode_put(program);
 fajita_appos_stage = 8;
 if (mode == 2 && fajita_appos_os_link == 0) fajita_appos_stage = 9;
done:
 fajita_appos_status = error;
 printf("fajita appos: mode %d stage %d status %d device %s os-link %d\n", mode, fajita_appos_stage, error,
        fajita_appos_device[0] ? fajita_appos_device : "-", fajita_appos_os_link);
 return fajita_appos_stage >= 8;
}
