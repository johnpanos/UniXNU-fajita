// fjbundle --ls DIR...: list directories (iOS here has no ls); each prints its entry count and names.
// fjbundle --stat PATH...: lstat/stat flags and open(2) results per path.
// fjbundle: probe CFBundle's view of a framework, as WebKit's XPC service entry does it (CFBundleGetBundleWithIdentifier then
// CFBundleGetFunctionPointerForName). Usage: fjbundle FRAMEWORK-BINARY-PATH BUNDLE-ID [SYMBOL]
//   session.sh push .cache/fajita-inputs/fjbundle /private/var/ngpu/sbin/fjbundle
//   session.sh run /usr/sbin/fjbundle /System/Library/Frameworks/WebKit.framework/WebKit com.apple.WebKit WebContentServiceInitializer
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

extern int __getdirentries64(int, void *, size_t, off_t *);

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "--ls")) {
    for (int i = 2; i < argc; i++) {
      DIR *d = opendir(argv[i]); int n = 0;
      if (!d) { printf("%s: cannot open: %s\n", argv[i], strerror(errno)); continue; }
      struct dirent *e; char line[2048] = "";
      while ((e = readdir(d))) { if (n++ < 40) { strncat(line, " ", sizeof line - strlen(line) - 1); strncat(line, e->d_name, sizeof line - strlen(line) - 1); } }
      closedir(d);
      printf("%s: %d entries:%s\n", argv[i], n, line);
    }
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "--stat")) {
    for (int i = 2; i < argc; i++) {
      struct stat st;
      if (lstat(argv[i], &st)) { printf("%s: lstat: %s\n", argv[i], strerror(errno)); continue; }
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
