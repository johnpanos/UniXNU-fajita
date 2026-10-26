// fjbundle --ls DIR...: list directories (iOS here has no ls); each prints its entry count and names.
// fjbundle --stat PATH...: lstat/stat flags and open(2) results per path.
// fjbundle --find DIR [MAXDEPTH]: every file under DIR (default depth 8): size, mtime (s, and seconds from now), data protection
//   class (F_GETPROTECTIONCLASS; ? = open failed), flags; the first line is the clock (time(), gettimeofday). For stores that grow
//   (Biome segments under /private/var/mobile/Library/Biome) and timestamps from before/after a clock jump.
// fjbundle --settime UNIXSECONDS|now+N: settimeofday, as timed does after a network time fix (to reproduce what a clock jump does to
//   daemons on a boot without Wi-Fi); prints the time before and after.
// fjbundle --lsrecord [--mobile] BUNDLEID...: LaunchServices' view of an app, as SafariCore asks it for MobileSafari's data
//   container: LSApplicationRecord initWithBundleIdentifier:allowPlaceholder:NO error: (URL, dataContainerURL, the error), and
//   LSApplicationProxy applicationProxyForIdentifier: (bundleURL, dataContainerURL, applicationType); --mobile as uid 501.
// fjbundle --container PID|NAME...: sandbox_container_path_for_pid as libsystem_sandbox's callers ask it (SafariCore, PlugInKit):
//   the return value, errno and the path; an app or extension (CFFIXED_USER_HOME set) gets its container, a daemon ENOENT.
// fjbundle --fds PID|NAME: the open vnodes of a process (proc_pidinfo PROC_PIDLISTFDS + PROC_PIDFDVNODEPATHINFO): fd, offset, size, path.
// fjbundle --lookup [--ping] [--watch SUBSTR] SERVICE...: bootstrap_look_up each Mach service (a job disabled in launchd's overrides gives
//   BOOTSTRAP_UNKNOWN_SERVICE 1102); --ping also sends it an empty XPC message and waits up to 3 s for any answer, which makes launchd
//   start an on-demand job; --watch samples the process table every 20 ms meanwhile for an executable path containing SUBSTR (an
//   on-demand job may exit again before ps sees it).
// fjbundle --rm PATH...: unlink files, then rmdir directories (iOS here has no rm), each with its result.
// fjbundle --kill SIG NAME|PID: send SIG (a number) to a process, by pid or by the last path component of its executable (the
//   first match), e.g. --kill 9 backboardd for the compositor-restart test; prints the pid.
// fjbundle --prefs APPID KEY=0|1|d...: become mobile (uid 501) and set (d: remove) boolean preferences of APPID through cfprefsd.
// fjbundle --open URL: open URL the way SpringBoard does (LaunchServices' LSApplicationWorkspace openSensitiveURL:withOptions:, then
//   openURL: if that is refused), e.g. --open https://news.ycombinator.com/ brings Safari to the front on that page; prints the result.
// fjbundle: probe CFBundle's view of a framework, as WebKit's XPC service entry does it (CFBundleGetBundleWithIdentifier then
// CFBundleGetFunctionPointerForName). Usage: fjbundle FRAMEWORK-BINARY-PATH BUNDLE-ID [SYMBOL]
//   session.sh push .cache/fajita-inputs/fjbundle /private/var/ngpu/sbin/fjbundle
//   session.sh run /usr/sbin/fjbundle /System/Library/Frameworks/WebKit.framework/WebKit com.apple.WebKit WebContentServiceInitializer
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <mach/mach.h>
// servers/bootstrap.h is not in the CI SDK
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
#include <dispatch/dispatch.h>

// libxpc (in libSystem); the connection constructor is marked unavailable on iOS in the SDK headers, so these are declared here.
typedef void *fj_xpc_t;
extern fj_xpc_t fj_xpc_create_mach_service(const char *, dispatch_queue_t, uint64_t) __asm__("_xpc_connection_create_mach_service");
extern void xpc_connection_set_event_handler(fj_xpc_t, void (^)(fj_xpc_t));
extern void xpc_connection_resume(fj_xpc_t);
extern void xpc_connection_cancel(fj_xpc_t);
extern fj_xpc_t xpc_dictionary_create(const char *const *, const fj_xpc_t *, size_t);
extern void xpc_connection_send_message_with_reply(fj_xpc_t, fj_xpc_t, dispatch_queue_t, void (^)(fj_xpc_t));
extern char *xpc_copy_description(fj_xpc_t);
extern int proc_listallpids(void *, int);
extern int proc_pidpath(int, void *, uint32_t);

// The first process whose executable path contains `what`, or 0.
static int find_process(const char *what) {
  static int pids[4096]; char path[1024];
  int n = proc_listallpids(pids, sizeof pids);
  for (int i = 0; i < n && i < 4096; i++)
    if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof path) > 0 && strstr(path, what)) return pids[i];
  return 0;
}

extern int __getdirentries64(int, void *, size_t, off_t *);
extern int sandbox_container_path_for_pid(pid_t, char *, size_t);

// sys/proc_info.h is not in the iPhoneOS SDK: the parts --fds uses (xnu bsd/sys/proc_info.h).
extern int proc_pidinfo(int, int, uint64_t, void *, int);
extern int proc_pidfdinfo(int, int, int, void *, int);
struct fj_proc_fdinfo { int32_t proc_fd; uint32_t proc_fdtype; };
struct fj_vinfo_stat { uint32_t dev; uint16_t mode, nlink; uint64_t ino; uid_t uid; gid_t gid; int64_t at, atn, mt, mtn, ct, ctn, bt, btn;
  off_t size; int64_t blocks; int32_t blksize; uint32_t flags, gen, rdev; int64_t qspare[2]; };
struct fj_vnode_fdinfowithpath { uint32_t openflags, status; off_t offset; int32_t type; uint32_t guardflags;
  struct fj_vinfo_stat st; int vtype, vpad; fsid_t fsid; char path[1024]; };

static time_t find_now;
static void find_walk(const char *dir, int depth, int maxdepth, long long *files, long long *bytes) {
  DIR *d = opendir(dir);
  if (!d) { printf("%s/: cannot open: %s\n", dir, strerror(errno)); return; }
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(p, &st)) { printf("%s: lstat: %s\n", p, strerror(errno)); continue; }
    if (S_ISDIR(st.st_mode)) {
      printf("%s/ mode %o mtime %ld (%+ld)\n", p, st.st_mode & 07777, (long)st.st_mtime, (long)(st.st_mtime - find_now));
      if (depth < maxdepth) find_walk(p, depth + 1, maxdepth, files, bytes);
      continue;
    }
    int pc = -2, fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) { pc = fcntl(fd, F_GETPROTECTIONCLASS); close(fd); }
    char pcs[16]; if (pc == -2) snprintf(pcs, sizeof pcs, "?(%d)", errno); else snprintf(pcs, sizeof pcs, "%d", pc);
    printf("%s size %lld mtime %ld (%+ld) birth %ld pclass %s flags %#x mode %o uid %d\n", p, (long long)st.st_size,
           (long)st.st_mtime, (long)(st.st_mtime - find_now), (long)st.st_birthtimespec.tv_sec, pcs, st.st_flags, st.st_mode & 07777, st.st_uid);
    (*files)++; *bytes += st.st_size;
  }
  closedir(d);
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--kill")) {
    int sig = atoi(argv[2]), pid = atoi(argv[3]);
    if (!pid) {
      static int pids[4096]; char path[1024], want[1100];
      snprintf(want, sizeof want, "/%s", argv[3]);
      int n = proc_listallpids(pids, sizeof pids);
      for (int i = 0; i < n && i < 4096 && !pid; i++)
        if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof path) > 0 && strlen(path) >= strlen(want) &&
            !strcmp(path + strlen(path) - strlen(want), want)) pid = pids[i];
    }
    if (pid <= 1) { printf("%s: no such process\n", argv[3]); return 1; }
    int r = kill(pid, sig);
    printf("kill %d %d: %s\n", sig, pid, r ? strerror(errno) : "sent");
    return r ? 1 : 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--rm")) {
    int bad = 0;
    for (int i = 2; i < argc; i++) {
      struct stat st; int r = lstat(argv[i], &st) ? -1 : S_ISDIR(st.st_mode) ? rmdir(argv[i]) : unlink(argv[i]);
      printf("%s: %s\n", argv[i], r ? strerror(errno) : "removed"); bad += r != 0;
    }
    return bad ? 1 : 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--lookup")) {
    int at = 2, ping = 0, bad = 0; const char *watch = NULL;
    for (; at < argc && !strncmp(argv[at], "--", 2); at++) {
      if (!strcmp(argv[at], "--ping")) ping = 1;
      else if (!strcmp(argv[at], "--watch") && at + 1 < argc) watch = argv[++at];
    }
    for (int i = at; i < argc; i++) {
      mach_port_t port = MACH_PORT_NULL;
      kern_return_t kr = bootstrap_look_up(bootstrap_port, argv[i], &port);
      printf("%s: look_up %d%s", argv[i], kr, kr == 0 ? " (found)" : kr == 1102 ? " (unknown service: disabled or not loaded)" : "");
      if (kr) bad++;
      if (!kr && ping) {
        dispatch_queue_t q = dispatch_queue_create("fjbundle.ping", NULL);
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        fj_xpc_t c = fj_xpc_create_mach_service(argv[i], q, 0);
        xpc_connection_set_event_handler(c, ^(fj_xpc_t e) { (void)e; });
        xpc_connection_resume(c);
        char *answer = calloc(1, 160); strcpy(answer, "none");
        xpc_connection_send_message_with_reply(c, xpc_dictionary_create(NULL, NULL, 0), q, ^(fj_xpc_t r) {
          char *d = xpc_copy_description(r); snprintf(answer, 160, "%.150s", d ? d : "?"); free(d);
          dispatch_semaphore_signal(done); });
        long waited = 1; int seen = 0;
        for (int tick = 0; tick < 150 && waited; tick++) {   // 3 s in 20 ms steps, sampling the process table for --watch
          waited = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
          if (watch && !seen) seen = find_process(watch);
        }
        if (watch) printf("; %s pid %d%s", watch, seen, seen ? " (ran)" : " (not seen)");
        printf("; ping %s: %s", waited ? "no answer in 3 s" : "answered", answer);
        xpc_connection_cancel(c);
        if (!waited) free(answer);   // a late reply block may still write it otherwise
      }
      printf("\n");
      if (port) mach_port_deallocate(mach_task_self(), port);
    }
    return bad ? 1 : 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--ls")) {
    for (int i = 2; i < argc; i++) {
      DIR *d = opendir(argv[i]); int n = 0;
      if (!d) { printf("%s: cannot open: %s\n", argv[i], strerror(errno)); continue; }
      struct dirent *e; char line[65536] = "";
      while ((e = readdir(d))) { n++; { strncat(line, " ", sizeof line - strlen(line) - 1); strncat(line, e->d_name, sizeof line - strlen(line) - 1); } }
      closedir(d);
      printf("%s: %d entries:%s\n", argv[i], n, line);
    }
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--find")) {
    struct timeval tv; gettimeofday(&tv, NULL); find_now = time(NULL);
    printf("now time() %ld gettimeofday %ld.%06d\n", (long)find_now, (long)tv.tv_sec, (int)tv.tv_usec);
    long long files = 0, bytes = 0;
    find_walk(argv[2], 0, argc > 3 ? atoi(argv[3]) : 8, &files, &bytes);
    printf("%s: %lld files, %lld bytes\n", argv[2], files, bytes);
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--lsrecord")) {
    int at = 2;
    if (!strcmp(argv[at], "--mobile")) { if (setgid(501) || setuid(501)) { perror("setuid(501)"); return 1; } at++; }
    if (!dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW) &&
        !dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW)) { printf("dlopen CoreServices: %s\n", dlerror()); return 1; }
    void *(*getClass)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!getClass || !sel || !send) { printf("no objc runtime\n"); return 1; }
    void *(*msg0)(void *, void *) = (void *(*)(void *, void *))send;
    void *(*msg1)(void *, void *, const void *) = (void *(*)(void *, void *, const void *))send;
    void *(*msg3)(void *, void *, const void *, long, void **) = (void *(*)(void *, void *, const void *, long, void **))send;
    #define FJ_DESC(o) ({ char _b[2048] = "(nil)"; if (o) { CFStringRef _d = CFCopyDescription((CFTypeRef)(o)); if (_d) { CFStringGetCString(_d, _b, sizeof _b, kCFStringEncodingUTF8); CFRelease(_d); } } _b; })
    printf("uid %d\n", getuid());
    for (; at < argc; at++) {
      CFStringRef id = CFStringCreateWithCString(NULL, argv[at], kCFStringEncodingUTF8);
      void *rc = getClass("LSApplicationRecord"), *err = NULL, *rec = NULL;
      if (rc) rec = msg3(msg0(rc, sel("alloc")), sel("initWithBundleIdentifier:allowPlaceholder:error:"), id, 0, &err);
      printf("%s: LSApplicationRecord %s\n", argv[at], rec ? "found" : "nil");
      if (err) printf("  error: %s\n", FJ_DESC(err));
      if (rec) {
        printf("  URL: %s\n", FJ_DESC(msg0(rec, sel("URL"))));
        printf("  dataContainerURL: %s\n", FJ_DESC(msg0(rec, sel("dataContainerURL"))));
        printf("  isPlaceholder: %ld\n", (long)(signed char)(intptr_t)msg0(rec, sel("isPlaceholder")));
      }
      void *pc = getClass("LSApplicationProxy");
      void *px = pc ? msg1(pc, sel("applicationProxyForIdentifier:"), id) : NULL;
      if (px) {
        printf("  proxy bundleURL: %s\n", FJ_DESC(msg0(px, sel("bundleURL"))));
        printf("  proxy dataContainerURL: %s\n", FJ_DESC(msg0(px, sel("dataContainerURL"))));
        printf("  proxy applicationType: %s\n", FJ_DESC(msg0(px, sel("applicationType"))));
        printf("  proxy isInstalled: %ld\n", (long)(signed char)(intptr_t)msg0(px, sel("isInstalled")));
      } else printf("  LSApplicationProxy nil\n");
      CFRelease(id);
    }
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--container")) {
    for (int i = 2; i < argc; i++) {
      int pid = atoi(argv[i]); if (pid <= 0) pid = find_process(argv[i]);
      if (pid <= 0) { printf("%s: no process\n", argv[i]); continue; }
      char buf[1024]; memset(buf, 0, sizeof buf); buf[0] = '?';
      errno = 0; int r = sandbox_container_path_for_pid(pid, buf, sizeof buf); int e = errno;
      printf("%s pid %d: rc %d errno %d (%s) path '%s'\n", argv[i], pid, r, e, e ? strerror(e) : "-", buf);
    }
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--settime")) {
    struct timeval before, tv = {0, 0}; gettimeofday(&before, NULL);
    tv.tv_sec = !strncmp(argv[2], "now+", 4) ? before.tv_sec + atol(argv[2] + 4) : atol(argv[2]);
    int r = settimeofday(&tv, NULL); int e = errno;
    struct timeval after; gettimeofday(&after, NULL);
    printf("settimeofday(%ld) = %d (%s): before %ld, after %ld\n", (long)tv.tv_sec, r, r ? strerror(e) : "ok", (long)before.tv_sec, (long)after.tv_sec);
    return r ? 1 : 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--fds")) {
    int pid = atoi(argv[2]); if (pid <= 0) pid = find_process(argv[2]);
    if (pid <= 0) { printf("no process %s\n", argv[2]); return 1; }
    static struct fj_proc_fdinfo fds[4096];
    int n = proc_pidinfo(pid, 1 /* PROC_PIDLISTFDS */, 0, fds, sizeof fds);
    if (n <= 0) { printf("pid %d: PROC_PIDLISTFDS: %s\n", pid, strerror(errno)); return 1; }
    n /= (int)sizeof fds[0];
    printf("pid %d: %d fds\n", pid, n);
    for (int i = 0; i < n; i++) {
      if (fds[i].proc_fdtype != 1 /* PROX_FDTYPE_VNODE */) { printf("  fd %d type %u\n", fds[i].proc_fd, fds[i].proc_fdtype); continue; }
      struct fj_vnode_fdinfowithpath vi;
      int r = proc_pidfdinfo(pid, fds[i].proc_fd, 2 /* PROC_PIDFDVNODEPATHINFO */, &vi, sizeof vi);
      if (r != (int)sizeof vi) { printf("  fd %d vnode: %d (%s)\n", fds[i].proc_fd, r, strerror(errno)); continue; }
      printf("  fd %d off %lld size %lld oflags %#x %s\n", fds[i].proc_fd, (long long)vi.offset, (long long)vi.st.size, vi.openflags, vi.path);
    }
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--stat")) {
    for (int i = 2; i < argc; i++) {
      struct stat st;
      if (lstat(argv[i], &st)) { printf("%s: lstat: %s\n", argv[i], strerror(errno)); continue; }
      if (S_ISLNK(st.st_mode)) { char lb[1024]; ssize_t ln = readlink(argv[i], lb, sizeof lb - 1); if (ln >= 0) { lb[ln] = 0; printf("%s -> %s\n", argv[i], lb); } }
      int fd = open(argv[i], O_RDONLY); int e1 = errno;
      int fd2 = open(argv[i], O_RDONLY | O_DIRECTORY); int e2 = errno;
      printf("%s: mode %o flags %#x size %lld nlink %d ino %llu; open=%d(%s) open|O_DIRECTORY=%d(%s)\n", argv[i], st.st_mode, st.st_flags,
             (long long)st.st_size, st.st_nlink, (unsigned long long)st.st_ino, fd, fd < 0 ? strerror(e1) : "ok", fd2, fd2 < 0 ? strerror(e2) : "ok");
      struct statfs sf;
      if (!statfs(argv[i], &sf)) printf("  fs %s on %s from %s\n", sf.f_fstypename, sf.f_mntonname, sf.f_mntfromname);
      if (fd >= 0) {
        int dfd = dup(fd); DIR *dd = fdopendir(dfd); int e3 = errno; struct dirent *de = NULL; int cnt = 0;
        if (dd) { errno = 0; while ((de = readdir(dd))) cnt++; e3 = errno; closedir(dd); }
        { char buf[16384]; off_t base = 0;
          for (int k = 0; k < 3; k++) {
            int n = __getdirentries64(fd, buf, sizeof buf, &base);
            printf("  getdirentries64 #%d = %d (%s)\n", k, n, n < 0 ? strerror(errno) : "ok");
            if (n <= 0) break;
          } }
        printf("  fdopendir=%s readdir count %d errno %d (%s)\n", dd ? "ok" : "FAIL", cnt, e3, strerror(e3));
      }
      if (fd >= 0) close(fd);
      if (fd2 >= 0) close(fd2);
    }
    return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "--prefs")) {
    if (setgid(501) || setuid(501)) { perror("setuid(501)"); return 1; }
    CFStringRef app = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    for (int i = 3; i < argc; i++) {
      char *eq = strchr(argv[i], '='); if (!eq) continue;
      *eq = 0;
      CFStringRef k = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
      CFPreferencesSetAppValue(k, eq[1] == 'd' ? NULL : eq[1] == '1' ? kCFBooleanTrue : kCFBooleanFalse, app);
      printf("%s %s = %c\n", argv[2], argv[i], eq[1]);
      CFPreferencesAppSynchronize(app);   // per key: with one synchronize at the end only the first of several removals reached the plist
    }
    printf("synchronize: %d\n", CFPreferencesAppSynchronize(app));
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--open")) {
    // No headers for the private class: reach it through the Objective-C runtime. A CFURL is toll-free bridged to NSURL.
    if (!dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW)) {
      printf("dlopen MobileCoreServices: %s\n", dlerror()); return 1;
    }
    void *(*getClass)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *ws = getClass && sel && send ? getClass("LSApplicationWorkspace") : NULL;
    if (!ws) { printf("no LSApplicationWorkspace\n"); return 1; }
    ws = ((void *(*)(void *, void *))send)(ws, sel("defaultWorkspace"));
    CFStringRef s = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    CFURLRef url = s ? CFURLCreateWithString(NULL, s, NULL) : NULL;
    if (!ws || !url) { printf("workspace %p url %p\n", ws, (void *)url); return 1; }
    int ok = (int)((signed char (*)(void *, void *, const void *, void *))send)(ws, sel("openSensitiveURL:withOptions:"), url, NULL);
    printf("openSensitiveURL:withOptions:(%s) = %d\n", argv[2], ok);
    if (!ok) {
      ok = (int)((signed char (*)(void *, void *, const void *))send)(ws, sel("openURL:"), url);
      printf("openURL:(%s) = %d\n", argv[2], ok);
    }
    return ok ? 0 : 1;
  }
  if (argc < 3) { fprintf(stderr, "usage: fjbundle FRAMEWORK-BINARY BUNDLE-ID [SYMBOL]\n"); return 2; }
  void *h = dlopen(argv[1], RTLD_NOW);
  printf("dlopen(%s) = %p %s\n", argv[1], h, h ? "" : dlerror());
  {  // what the file system says about the framework directory and its Info.plist
    char dir[1024]; strncpy(dir, argv[1], sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    DIR *d = opendir(dir); int n = 0;
    if (d) { struct dirent *e; while ((e = readdir(d))) n++; closedir(d); }
    char ip[1100]; snprintf(ip, sizeof ip, "%s/Info.plist", dir);
    FILE *f = fopen(ip, "rb"); char b8[8] = {0}; size_t got = f ? fread(b8, 1, 8, f) : 0;
    printf("dir %s: %d entries; %s: %s, read %zu bytes %.8s\n", dir, n, ip, f ? "opens" : "does not open", got, b8);
    if (f) fclose(f);
  }
  CFStringRef id = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
  CFBundleRef b = CFBundleGetBundleWithIdentifier(id);
  printf("CFBundleGetBundleWithIdentifier(%s) = %p\n", argv[2], (void *)b);
  {
    char dir[1024]; strncpy(dir, argv[1], sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *s = strrchr(dir, '/'); if (s) *s = 0;
    CFStringRef p = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, true);
    CFBundleRef c = CFBundleCreate(NULL, u);
    printf("CFBundleCreate(%s) = %p\n", dir, (void *)c);
    if (c) {
      CFStringRef cid = CFBundleGetIdentifier(c);
      char buf[256] = "(null)"; if (cid) CFStringGetCString(cid, buf, sizeof buf, kCFStringEncodingUTF8);
      printf("  created bundle id %s\n", buf);
      CFURLRef eu = CFBundleCopyExecutableURL(c);
      CFStringRef es = eu ? CFURLCopyFileSystemPath(eu, kCFURLPOSIXPathStyle) : NULL;
      if (es) CFStringGetCString(es, buf, sizeof buf, kCFStringEncodingUTF8); else strcpy(buf, "(null)");
      printf("  executable %s\n", buf);
    }
    b = c;
  }
  if (b && argc > 3) {
    CFStringRef n = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
    printf("CFBundleGetFunctionPointerForName(%s) = %p\n", argv[3], CFBundleGetFunctionPointerForName(b, n));
  }
  return 0;
}
