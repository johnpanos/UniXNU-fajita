// fjsurface: look at the IOSurfaces of every process (iOS here has no IOSurface tool).
//   fjsurface --scan [MAXID] [MINBYTES]  one line per live surface with an ID up to MAXID (default 8192) and at least MINBYTES
//                                        (default 65536): size, pixel format, planes, row bytes, allocation, purgeable state, seed,
//                                        and the plane-0 pixels counted (zero words, 0xffffffff words, other), read under a read-only lock
//   fjsurface --dump ID FILE             the surface's whole allocation, raw, to FILE (pull it and convert on the host)
//   fjsurface --share [W H [FOURCC]]     cross-process sharing test: a child creates a surface, writes a pattern and sends its Mach port
//                                        (IOSurfaceCreateMachPort) to the parent, which looks it up, checks the pattern and writes its own;
//                                        the child checks that. Prints the addresses, physical-page independent results, PASS or FAIL.
//   fjsurface --cg FOURCC [W H [INFO]]        draw with CoreGraphics into a new surface of that format, as WebKit's accelerated drawing does
//                                        (CGIOSurfaceContextCreate; w30r = RGB10 with kCGImagePixelFormatRGBCIF10, BGRA = 8-bit BGRA): fill white,
//                                        a black square, flush, then count the words (zero / other) and print the seed.
//   fjsurface --images PID [FILTER]      the images dyld loaded in another process (task_for_pid; whether Metal and the GPU plugin are in it)
//   fjsurface --sample PID SECONDS [HZ [all]]   sample another process's threads (stacks by count, unslid for tools/crash/dsc-sym.py
//                                        --slide); all: waiting threads too (who is blocked in an XPC call to a daemon)
// (IOSurfaceLookup by ID fails here for every surface: the kernel logs "IOSurface disallowing global lookups", as on any iPhone.)
// Build: scripts/fajita/build-usbdbg-agent.sh; push with session.sh push (to a new name when the binary changed).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);

static void fourcc(uint32_t f, char out[5]) {
  for (int i = 0; i < 4; i++) { char c = (char)(f >> (24 - 8 * i)); out[i] = (c >= 32 && c < 127) ? c : '.'; }
  out[4] = 0;
}

static const char *purgeable(IOSurfaceRef s) {
  uint32_t old = 99;
  if (IOSurfaceSetPurgeable(s, kIOSurfacePurgeableKeepCurrent, &old)) return "?";
  switch (old) {
  case kIOSurfacePurgeableNonVolatile: return "nonvol";
  case kIOSurfacePurgeableVolatile: return "VOL";
  case kIOSurfacePurgeableEmpty: return "EMPTY";
  default: return "?";
  }
}

static void name(IOSurfaceRef s, char *out, size_t size) {
  out[0] = 0;
  CFTypeRef v = IOSurfaceCopyValue(s, CFSTR("IOSurfaceName"));
  if (!v) v = IOSurfaceCopyValue(s, CFSTR("name"));
  if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, out, (CFIndex)size, kCFStringEncodingUTF8);
  if (v) CFRelease(v);
}

static int scan(uint32_t maxid, size_t minbytes) {
  unsigned found = 0;
  for (uint32_t id = 1; id <= maxid; id++) {
    IOSurfaceRef s = IOSurfaceLookup(id);
    if (!s) continue;
    const size_t alloc = IOSurfaceGetAllocSize(s);
    if (alloc < minbytes) { CFRelease(s); continue; }
    found++;
    char f[5], n[64];
    fourcc(IOSurfaceGetPixelFormat(s), f); name(s, n, sizeof n);
    const size_t planes = IOSurfaceGetPlaneCount(s), w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), row = IOSurfaceGetBytesPerRow(s);
    unsigned long zero = 0, white = 0, other = 0; uint32_t sample = 0; int locked = 0;
    if (!IOSurfaceLock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL)) {
      locked = 1;
      const uint8_t *base = IOSurfaceGetBaseAddressOfPlane(s, 0);
      if (!planes) base = IOSurfaceGetBaseAddress(s);
      const size_t pw = planes ? IOSurfaceGetWidthOfPlane(s, 0) : w, ph = planes ? IOSurfaceGetHeightOfPlane(s, 0) : h;
      const size_t prow = planes ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : row;
      const size_t bpe = planes ? IOSurfaceGetBytesPerElementOfPlane(s, 0) : IOSurfaceGetBytesPerElement(s);
      const size_t words = bpe ? pw * bpe / 4 : prow / 4;
      for (size_t y = 0; base && y < ph; y++) {
        const uint32_t *p = (const uint32_t *)(base + y * prow);
        for (size_t x = 0; x < words; x++) {
          if (!p[x]) zero++; else if (p[x] == 0xffffffffu) white++; else { other++; if (!sample) sample = p[x]; }
        }
      }
      IOSurfaceUnlock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    }
    const unsigned long all = zero + white + other;
    printf("id %u %zux%zu %s pl%zu rb%zu alloc %zu %s seed %u", id, w, h, f, planes, row, alloc, purgeable(s), IOSurfaceGetSeed(s));
    if (locked && all) printf(" px zero %lu%% white %lu%% other %lu%% (first %08x)", zero * 100 / all, white * 100 / all, other * 100 / all, sample);
    else printf(" px unreadable");
    if (n[0]) printf(" name '%s'", n);
    printf("\n");
    CFRelease(s);
  }
  printf("%u surfaces of at least %zu bytes, ids 1..%u\n", found, minbytes, maxid);
  return 0;
}

static int dump(uint32_t id, const char *path) {
  IOSurfaceRef s = IOSurfaceLookup(id);
  if (!s) { printf("no surface %u\n", id); return 1; }
  FILE *o = fopen(path, "wb");
  if (!o) { perror(path); CFRelease(s); return 1; }
  if (IOSurfaceLock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL)) { printf("lock failed\n"); fclose(o); CFRelease(s); return 1; }
  const size_t alloc = IOSurfaceGetAllocSize(s);
  const size_t wrote = fwrite(IOSurfaceGetBaseAddress(s), 1, alloc, o);
  IOSurfaceUnlock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
  fclose(o);
  char f[5]; fourcc(IOSurfaceGetPixelFormat(s), f);
  printf("id %u %zux%zu %s rb%zu planes %zu: %zu of %zu bytes to %s\n", id, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), f,
         IOSurfaceGetBytesPerRow(s), IOSurfaceGetPlaneCount(s), wrote, alloc, path);
  CFRelease(s);
  return wrote == alloc ? 0 : 1;
}


typedef struct {mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t port; uint32_t id;} ShareMsg;
typedef struct {ShareMsg m; mach_msg_trailer_t trailer;} ShareRcv;

static int quiet;
static unsigned check(IOSurfaceRef s, uint32_t salt, int write) {
  unsigned bad = 0;
  if (IOSurfaceLock(s, write ? 0 : kIOSurfaceLockReadOnly, NULL)) return ~0u;
  uint8_t *base = IOSurfaceGetBaseAddress(s);
  const size_t row = IOSurfaceGetBytesPerRow(s), h = IOSurfaceGetHeight(s), words = IOSurfaceGetWidth(s) * IOSurfaceGetBytesPerElement(s) / 4;
  for (size_t y = 0; y < h; y++) {
    uint32_t *p = (uint32_t *)(base + y * row);
    for (size_t x = 0; x < words; x++) {
      const uint32_t want = (uint32_t)(y * 4096 + x) ^ salt;
      if (write) p[x] = want; else if (p[x] != want) { if (!bad && !quiet) printf("  first mismatch at row %zu word %zu: %08x, want %08x\n", y, x, p[x], want); bad++; }
    }
  }
  IOSurfaceUnlock(s, write ? 0 : kIOSurfaceLockReadOnly, NULL);
  return bad;
}

static int share(int w, int h, uint32_t format) {
  mach_port_t rx = MACH_PORT_NULL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rx) ||
      mach_port_insert_right(mach_task_self(), rx, rx, MACH_MSG_TYPE_MAKE_SEND)) { printf("FAIL port\n"); return 1; }
  // The child inherits the bootstrap special port across fork: hand it ours for the fork, then restore it.
  mach_port_t bootstrap = MACH_PORT_NULL;
  task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bootstrap);
  if (task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, rx)) { printf("FAIL task_set_special_port\n"); return 1; }
  const pid_t child = fork();
  if (child) task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, bootstrap);
  if (child < 0) { perror("fork"); return 1; }
  if (!child) {
    mach_port_t parent = MACH_PORT_NULL;
    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &parent) || !parent) { printf("child: FAIL no parent port\n"); _exit(1); }
    const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    int bpe = 4;
    CFNumberRef vals[] = {CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
                          CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberSInt32Type, &format)};
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, (const void **)vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(props);
    if (!s) { printf("child: FAIL IOSurfaceCreate\n"); _exit(1); }
    check(s, 0x11111111u, 1);
    printf("child %d: surface %u %dx%d rb%zu alloc %zu at %p, wrote pattern A, seed %u\n", getpid(), IOSurfaceGetID(s), w, h,
           IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetSeed(s));
    ShareMsg m = {0};
    m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    m.header.msgh_size = sizeof m; m.header.msgh_remote_port = parent; m.body.msgh_descriptor_count = 1;
    m.port.name = IOSurfaceCreateMachPort(s); m.port.disposition = MACH_MSG_TYPE_MOVE_SEND; m.port.type = MACH_MSG_PORT_DESCRIPTOR;
    m.id = IOSurfaceGetID(s);
    const kern_return_t sent = mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL);
    if (sent) { printf("child: FAIL send (%#x, surface port %#x, parent port %#x)\n", sent, m.port.name, parent); _exit(1); }
    printf("child: sent the surface port\n");
    quiet = 1;
    for (int i = 0; i < 50; i++) {   // the parent writes pattern B within 5 s
      usleep(100000);
      if (!check(s, 0x22222222u, 0)) { printf("child: PASS sees the parent's pattern B (seed %u)\n", IOSurfaceGetSeed(s)); _exit(0); }
    }
    quiet = 0;
    printf("child: FAIL never saw pattern B; ");
    const unsigned a = check(s, 0x11111111u, 0);
    printf("pattern A mismatches %u of %d\n", a, w * h);
    _exit(1);
  }
  ShareRcv r = {0};
  const kern_return_t got = mach_msg(&r.m.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r, rx, 5000, MACH_PORT_NULL);
  if (got) { printf("parent: FAIL receive (%#x)\n", got); kill(child, 9); return 1; }
  IOSurfaceRef s = IOSurfaceLookupFromMachPort(r.m.port.name);
  if (!s) { printf("parent: FAIL IOSurfaceLookupFromMachPort\n"); kill(child, 9); return 1; }
  printf("parent %d: surface %u (child said %u) rb%zu alloc %zu at %p, seed %u\n", getpid(), IOSurfaceGetID(s), r.m.id, IOSurfaceGetBytesPerRow(s),
         IOSurfaceGetAllocSize(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetSeed(s));
  const unsigned bad = check(s, 0x11111111u, 0);
  printf("parent: %s pattern A (%u of %d words wrong)\n", bad ? "FAIL" : "PASS", bad, w * h);
  check(s, 0x22222222u, 1);
  printf("parent: wrote pattern B, seed %u\n", IOSurfaceGetSeed(s));
  int status = 0; waitpid(child, &status, 0);
  return (bad || !WIFEXITED(status) || WEXITSTATUS(status)) ? 1 : 0;
}

extern CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo);
static unsigned long cgInfoOverride;
static uint32_t imagesBefore;
static void newImages(const char *when) {   // the libraries CoreGraphics loaded for the drawing (its conversion path)
  const uint32_t n = _dyld_image_count();
  for (uint32_t i = imagesBefore; i < n; i++) printf("  %s loaded %s\n", when, _dyld_get_image_name(i));
  imagesBefore = n;
}
static int cgdraw(uint32_t format, int w, int h) {
  int bpe = 4;
  const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
  CFNumberRef vals[] = {CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
                        CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberSInt32Type, &format)};
  CFDictionaryRef props = CFDictionaryCreate(NULL, keys, (const void **)vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  IOSurfaceRef s = IOSurfaceCreate(props);
  if (!s) { printf("FAIL IOSurfaceCreate\n"); return 1; }
  const int rgb10 = format == 'w30r';
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(rgb10 ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
  CGBitmapInfo info = rgb10 ? (CGBitmapInfo)kCGImageAlphaNone | (CGBitmapInfo)kCGImagePixelFormatRGBCIF10 | (CGBitmapInfo)kCGImageByteOrder16Little
                                  : (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | (CGBitmapInfo)kCGImageByteOrder32Little;
  if (cgInfoOverride) info = (CGBitmapInfo)cgInfoOverride;
  printf("surface %u %dx%d rb%zu seed %u, bitmap info %#x\n", IOSurfaceGetID(s), w, h, IOSurfaceGetBytesPerRow(s), IOSurfaceGetSeed(s), (unsigned)info);
  imagesBefore = _dyld_image_count();
  CGContextRef c = CGIOSurfaceContextCreate(s, (size_t)w, (size_t)h, rgb10 ? 10 : 8, 32, cs, info);
  printf("CGIOSurfaceContextCreate = %p\n", (void *)c);
  if (!c) return 1;
  CGContextSetRGBFillColor(c, 1, 1, 1, 1); CGContextFillRect(c, CGRectMake(0, 0, w, h));
  CGContextSetRGBFillColor(c, 0, 0, 0, 1); CGContextFillRect(c, CGRectMake(0, 0, w / 4, h / 4));
  CGContextFlush(c);
  newImages("drawing");
  printf("after flush: seed %u\n", IOSurfaceGetSeed(s));
  IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
  const uint8_t *base = IOSurfaceGetBaseAddress(s); unsigned long zero = 0, other = 0; uint32_t first = 0;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    const uint32_t v = ((const uint32_t *)(base + (size_t)y * IOSurfaceGetBytesPerRow(s)))[x];
    if (!v) zero++; else { other++; if (!first) first = v; }
  }
  IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
  printf("words zero %lu other %lu (first %08x, corner %08x)\n", zero, other, first, *(const uint32_t *)base);
  CGContextRelease(c);
  newImages("release");
  {
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *b2 = IOSurfaceGetBaseAddress(s); unsigned long z2 = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) z2 += !((const uint32_t *)(b2 + (size_t)y * IOSurfaceGetBytesPerRow(s)))[x];
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    printf("after release: seed %u, words zero %lu of %d (corner %08x)\n", IOSurfaceGetSeed(s), z2, w * h, *(const uint32_t *)b2);
    if (z2 < zero) other = 1;
  }
  {  // the same drawing into plain memory (CGBitmapContextCreate): CoreGraphics' own rasterizer for the format, no IOSurface
    void *mem = calloc((size_t)w * 4, (size_t)h);
    CGContextRef b = CGBitmapContextCreate(mem, (size_t)w, (size_t)h, rgb10 ? 10 : 8, (size_t)w * 4, cs, info);
    unsigned long bz = 0;
    if (b) {
      CGContextSetRGBFillColor(b, 1, 1, 1, 1); CGContextFillRect(b, CGRectMake(0, 0, w, h)); CGContextFlush(b);
      for (size_t i = 0; i < (size_t)w * h; i++) bz += !((uint32_t *)mem)[i];
      CGContextRelease(b);
    }
    printf("CGBitmapContextCreate = %p: words zero %lu of %d (first %08x)\n", (void *)b, bz, w * h, *(uint32_t *)mem);
    free(mem);
  }
  return other ? 0 : 1;
}

static int images(int pid, const char *filter) {
  mach_port_t task = MACH_PORT_NULL;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr) { printf("task_for_pid(%d) failed: %#x\n", pid, kr); return 1; }
  struct task_dyld_info info; mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
  if ((kr = task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &count))) { printf("task_info failed: %#x\n", kr); return 1; }
  struct dyld_all_image_infos all; mach_vm_size_t got = 0;
  if ((kr = mach_vm_read_overwrite(task, info.all_image_info_addr, sizeof all, (mach_vm_address_t)&all, &got))) { printf("read infos: %#x\n", kr); return 1; }
  const uint32_t n = all.infoArrayCount; unsigned shown = 0;
  struct dyld_image_info *list = calloc(n, sizeof *list);
  if ((kr = mach_vm_read_overwrite(task, (mach_vm_address_t)all.infoArray, n * sizeof *list, (mach_vm_address_t)list, &got))) { printf("read list: %#x\n", kr); return 1; }
  for (uint32_t i = 0; i < n; i++) {
    char path[1024] = {0};
    if (mach_vm_read_overwrite(task, (mach_vm_address_t)list[i].imageFilePath, sizeof path - 1, (mach_vm_address_t)path, &got)) continue;
    if (filter && !strstr(path, filter)) continue;
    printf("%s\n", path); shown++;
  }
  printf("pid %d: %u images, %u shown\n", pid, n, shown);
  return 0;
}

// --sample PID SECONDS [HZ]: where another process's threads are (task_for_pid): every 1/HZ s (default 100) each thread is suspended, its
// pc, lr and up to 14 frame-pointer return addresses read, and resumed. Prints the shared cache slide, then per thread its samples, how
// many found it running, its name, and its distinct stacks by count (unslid: tools/crash/dsc-sym.py --slide SLIDE ADDR... names them).
// For a daemon that spins: what it is doing, without symbols on the phone.
#define SAMPLE_DEPTH 48
struct sample_stack { uint64_t thread; uint32_t count, depth; uint64_t pc[SAMPLE_DEPTH]; };
static int sample(int pid, double seconds, int hz, int all) {
  mach_port_t task = MACH_PORT_NULL;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr) { printf("task_for_pid(%d) failed: %#x\n", pid, kr); return 1; }
  struct task_dyld_info info; mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
  uint64_t slide = 0;
  if (!task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &count)) {
    struct dyld_all_image_infos all; mach_vm_size_t got = 0;
    if (!mach_vm_read_overwrite(task, info.all_image_info_addr, sizeof all, (mach_vm_address_t)&all, &got)) slide = all.sharedCacheSlide;
  }
  enum { kMax = 4096 };
  struct sample_stack *stacks = calloc(kMax, sizeof *stacks);
  unsigned nstacks = 0, overflow = 0, rounds = 0;
  struct { uint64_t id; uint32_t samples, running; char name[64]; } threads[256];
  unsigned nthreads = 0;
  const int n = (int)(seconds * hz);
  for (int round = 0; round < n; ++round, ++rounds) {
    thread_act_array_t list = NULL; mach_msg_type_number_t nlist = 0;
    if (task_threads(task, &list, &nlist)) break;
    for (mach_msg_type_number_t i = 0; i < nlist; ++i) {
      thread_identifier_info_data_t ident; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
      uint64_t id = thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &ic) ? (uint64_t)list[i] : ident.thread_id;
      thread_basic_info_data_t basic; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
      const int running = !thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &bc) && basic.run_state == TH_STATE_RUNNING;
      unsigned t = 0;
      while (t < nthreads && threads[t].id != id) ++t;
      if (t == nthreads && nthreads < 256) {
        threads[t].id = id; threads[t].samples = threads[t].running = 0; threads[t].name[0] = 0;
        thread_extended_info_data_t ext; mach_msg_type_number_t ec = THREAD_EXTENDED_INFO_COUNT;
        if (!thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ec)) snprintf(threads[t].name, sizeof threads[t].name, "%s", ext.pth_name);
        ++nthreads;
      }
      if (t < nthreads) { threads[t].samples++; threads[t].running += running; }
      uint64_t pcs[SAMPLE_DEPTH] = {0}; uint32_t depth = 0;
      if (!thread_suspend(list[i])) {
        arm_thread_state64_t st; mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
        if (!thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc)) {
          pcs[depth++] = __darwin_arm_thread_state64_get_pc(st) & 0xfffffffffULL;
          pcs[depth++] = __darwin_arm_thread_state64_get_lr(st) & 0xfffffffffULL;
          uint64_t fp = __darwin_arm_thread_state64_get_fp(st);
          while (fp && depth < SAMPLE_DEPTH) {
            uint64_t frame[2]; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(task, fp, sizeof frame, (mach_vm_address_t)frame, &got) || got != sizeof frame) break;
            if (!frame[1]) break;
            pcs[depth++] = frame[1] & 0xfffffffffULL;
            if (frame[0] <= fp) break;
            fp = frame[0];
          }
        }
        thread_resume(list[i]);
      }
      unsigned k = 0;
      for (; k < nstacks; ++k)
        if (stacks[k].thread == id && stacks[k].depth == depth && !memcmp(stacks[k].pc, pcs, depth * sizeof *pcs)) break;
      if (k == nstacks) { if (nstacks < kMax) { stacks[k].thread = id; stacks[k].depth = depth; memcpy(stacks[k].pc, pcs, sizeof pcs); ++nstacks; } else { ++overflow; continue; } }
      stacks[k].count += running || all ? 1 : 0;
      mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, nlist * sizeof *list);
    usleep((useconds_t)(1000000 / hz));
  }
  printf("sample pid %d: %u rounds over %.1f s, slide %#llx, %u threads, %u distinct stacks%s\n", pid, rounds, seconds, (unsigned long long)slide, nthreads,
         nstacks, overflow ? " (table full: some dropped)" : "");
  for (unsigned t = 0; t < nthreads; ++t) {
    if (!threads[t].running && !all) continue;
    printf("thread %#llx %s: running %u of %u samples\n", (unsigned long long)threads[t].id, threads[t].name[0] ? threads[t].name : "-", threads[t].running, threads[t].samples);
    for (unsigned shown = 0; shown < 6; ++shown) {   // its most frequent running stacks
      int best = -1;
      for (unsigned k = 0; k < nstacks; ++k)
        if (stacks[k].thread == threads[t].id && stacks[k].count && (best < 0 || stacks[k].count > stacks[best].count)) best = (int)k;
      if (best < 0) break;
      printf("  %u:", stacks[best].count);
      for (uint32_t d = 0; d < stacks[best].depth; ++d) printf(" %llx", (unsigned long long)(stacks[best].pc[d] - (stacks[best].pc[d] > slide ? slide : 0)));
      printf("\n");
      stacks[best].count = 0;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc >= 2 && !strcmp(argv[1], "--scan"))
    return scan(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 8192, argc > 3 ? (size_t)strtoull(argv[3], NULL, 0) : 65536);
  if (argc == 4 && !strcmp(argv[1], "--dump")) return dump((uint32_t)strtoul(argv[2], NULL, 0), argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "--cg") && strlen(argv[2]) == 4) {
    const uint32_t f = (uint32_t)argv[2][0] << 24 | (uint32_t)argv[2][1] << 16 | (uint32_t)argv[2][2] << 8 | (uint32_t)argv[2][3];
    if (argc > 5) cgInfoOverride = strtoul(argv[5], NULL, 0);
    return cgdraw(f, argc > 3 ? atoi(argv[3]) : 256, argc > 4 ? atoi(argv[4]) : 256);
  }
  if (argc >= 3 && !strcmp(argv[1], "--images")) return images(atoi(argv[2]), argc > 3 ? argv[3] : NULL);
  if (argc >= 4 && !strcmp(argv[1], "--sample")) return sample(atoi(argv[2]), atof(argv[3]), argc > 4 ? atoi(argv[4]) : 100, argc > 5 && !strcmp(argv[5], "all"));
  if (argc >= 2 && !strcmp(argv[1], "--share")) {
    uint32_t f = 'BGRA';
    if (argc > 4 && strlen(argv[4]) == 4) f = (uint32_t)argv[4][0] << 24 | (uint32_t)argv[4][1] << 16 | (uint32_t)argv[4][2] << 8 | (uint32_t)argv[4][3];
    return share(argc > 2 ? atoi(argv[2]) : 256, argc > 3 ? atoi(argv[3]) : 256, f);
  }
  fprintf(stderr, "usage: fjsurface --scan [MAXID] [MINBYTES] | --dump ID FILE | --share [W H [FOURCC]] | --cg FOURCC [W H [CGBITMAPINFO]] | --images PID [FILTER] | --sample PID SECONDS [HZ [all]]\n");
  return 2;
}
