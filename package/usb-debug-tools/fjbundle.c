// fjbundle --ls DIR...: list directories (iOS here has no ls); each prints its entry count and names.
// fjbundle --stat PATH...: lstat/stat flags and open(2) results per path.
// fjbundle --lookup [--ping] [--watch SUBSTR] SERVICE...: bootstrap_look_up each Mach service (a job disabled in launchd's overrides gives
//   BOOTSTRAP_UNKNOWN_SERVICE 1102); --ping also sends it an empty XPC message and waits up to 3 s for any answer, which makes launchd
//   start an on-demand job; --watch samples the process table every 20 ms meanwhile for an executable path containing SUBSTR (an
//   on-demand job may exit again before ps sees it).
// fjbundle --rm PATH...: unlink files, then rmdir directories (iOS here has no rm), each with its result.
// fjbundle --prefs APPID KEY=0|1|d...: become mobile (uid 501) and set (d: remove) boolean preferences of APPID through cfprefsd.
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
#include <unistd.h>
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

int main(int argc, char **argv) {
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
