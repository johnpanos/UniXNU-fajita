// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjdbg-agent: the userland end of the fajita USB debug channel. It is delivered as
// /usr/sbin/wifid through the boot-time addon union mount over /usr/sbin (boot arg
// usb-debug=3, kern_exec.c), so launchd's KeepAlive root job com.apple.wifid runs it
// on this Wi-Fi-less board. It opens /dev/fjdbg (the SHELL channel of NativeDwc3) and
// turns the host's lines into commands: each newline-terminated line is a program and
// its arguments, run with posix_spawn against the binaries iOS ships (ioreg, ps, df,
// hidutil, nvram, ...), its stdout+stderr streamed back to the host. A line of the
// form `>PATH\nbytes...` form is handled as a built-in (see below) so files can be read
// and written without a shell. No shell exists on iOS; this is the command runner that
// replaces one for touch-driver iteration.
//
// Built and 4 KiB ad-hoc signed on macOS CI (.github/workflows/usb-debug-agent.yml),
// trusted through the owner trust module and the AMFI stand-in (ad hoc = platform on
// this kernel). Plain C, libSystem only.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/sysctl.h>

extern char **environ;

#ifndef FJDBG_DEV
#define FJDBG_DEV "/dev/fjdbg"   // host tests point it at a pty
#endif

static int gDev = -1;
static int gTimeout = 60;   // seconds per command; `@t N CMD...` sets it for one command

static void writeAll(const char *buf, size_t len)
{
  while (len) {
    ssize_t n = write(gDev, buf, len);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    buf += n;
    len -= (size_t)n;
  }
}

static void say(const char *s) { writeAll(s, strlen(s)); }

// Split a command line into argv on spaces (no quoting; a debug channel, not a shell).
static int split(char *line, char **argv, int max)
{
  int n = 0;
  char *p = line;
  while (*p && n < max - 1) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    argv[n++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = '\0';
  }
  argv[n] = NULL;
  return n;
}

// Run argv[0] with argv, capturing stdout+stderr to the host. argv[0] may be a bare
// name (looked up under the usual iOS binary dirs) or an absolute path.
static void runCommand(char **argv)
{
  static const char *dirs[] = {"", "/usr/bin/", "/usr/sbin/", "/sbin/", "/bin/"};
  char path[1024];
  const char *prog = argv[0];
  if (prog[0] != '/') {
    for (size_t i = 1; i < sizeof dirs / sizeof dirs[0]; i++) {
      snprintf(path, sizeof path, "%s%s", dirs[i], prog);
      if (access(path, X_OK) == 0) {
        prog = path;
        break;
      }
    }
  }
  int fds[2];
  if (pipe(fds) != 0) {
    say("fjdbg: pipe failed\n");
    return;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  pid_t pid;
  int rc = posix_spawn(&pid, prog, &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    char msg[128];
    snprintf(msg, sizeof msg, "fjdbg: cannot run %s: %s\nfjdbg: exit 127\n", argv[0], strerror(rc));
    say(msg);
    close(fds[0]);
    return;
  }
  // Stream output until the command closes its end, but never longer than gTimeout seconds: a hung test is killed
  // instead of wedging the channel (the agent runs one command at a time).
  char buf[2048];
  ssize_t n;
  int timedOut = 0;
  time_t deadline = time(NULL) + gTimeout;
  for (;;) {
    int left = (int)(deadline - time(NULL));
    if (left <= 0) {
      kill(pid, SIGKILL);
      timedOut = 1;
      break;
    }
    struct pollfd pfd = {fds[0], POLLIN, 0};
    int r = poll(&pfd, 1, left * 1000);
    if (r < 0 && errno == EINTR)
      continue;
    if (r == 0)
      continue;
    n = read(fds[0], buf, sizeof buf);
    if (n <= 0)
      break;
    writeAll(buf, (size_t)n);
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  char tail[96];
  if (timedOut)
    snprintf(tail, sizeof tail, "fjdbg: timeout after %d s, killed\nfjdbg: exit 124\n", gTimeout);
  else
    snprintf(tail, sizeof tail, "fjdbg: exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  say(tail);
}

static uint32_t crc32Update(uint32_t crc, const unsigned char *p, size_t n)
{
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

static int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// File transfer, hex lines so the line-based channel carries any bytes.
//   @put PATH MODE SIZE   then `@d HEX` lines (each acked `fjdbg: k BYTES`), then `@end CRC32`: written to PATH.fjtmp, checked, chmod MODE (octal), renamed
//   @get PATH             replies `fjdbg: get SIZE`, `@d HEX` lines, `fjdbg: get crc CRC32`
//   @getr OFF LEN PATH    the same for LEN bytes from OFF (LEN 0: to the end), after `fjdbg: get total SIZE`, so the host pulls a
//                         large file in pieces. Both send exactly the SIZE they announce, so a file still being written cannot
//                         make the data disagree with the size (a trace file that keeps growing did).
static struct { int fd; char path[1024]; unsigned mode; long size, got; uint32_t crc; } gPut = {.fd = -1};

static void putFinish(const char *arg)
{
  char msg[1200];
  if (gPut.fd < 0) {
    say("fjdbg: put: no transfer\nfjdbg: eof\n");
    return;
  }
  uint32_t want = (uint32_t)strtoul(arg, NULL, 16);
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.fjtmp", gPut.path);
  int ok = gPut.got == gPut.size && gPut.crc == want && fchmod(gPut.fd, gPut.mode) == 0 && fsync(gPut.fd) == 0;
  close(gPut.fd);
  gPut.fd = -1;
  if (ok && rename(tmp, gPut.path) == 0)
    snprintf(msg, sizeof msg, "fjdbg: put ok %s %ld bytes crc %08x\nfjdbg: eof\n", gPut.path, gPut.got, gPut.crc);
  else {
    unlink(tmp);
    snprintf(msg, sizeof msg, "fjdbg: put failed %s: got %ld/%ld crc %08x want %08x (%s)\nfjdbg: eof\n", gPut.path, gPut.got,
             gPut.size, gPut.crc, want, strerror(errno));
  }
  say(msg);
}

static void putData(const char *hex)
{
  unsigned char buf[2048];
  size_t n = 0;
  if (gPut.fd < 0)
    return;
  for (; hex[0] && hex[1] && n < sizeof buf; hex += 2) {
    int hi = hexVal(hex[0]), lo = hexVal(hex[1]);
    if (hi < 0 || lo < 0)
      break;
    buf[n++] = (unsigned char)(hi << 4 | lo);
  }
  if (write(gPut.fd, buf, n) == (ssize_t)n) {
    gPut.crc = crc32Update(gPut.crc, buf, n);
    gPut.got += (long)n;
  }
  char ack[48];   // flow control: the host waits for this before overrunning NativeDwc3's 8 KiB shell ring
  snprintf(ack, sizeof ack, "fjdbg: k %ld\n", gPut.got);
  say(ack);
}

static void putStart(char *args)
{
  char *argv[4];
  char msg[1200];
  if (split(args, argv, 4) != 3) {
    say("fjdbg: usage: @put PATH MODE SIZE\nfjdbg: eof\n");
    return;
  }
  if (gPut.fd >= 0)
    close(gPut.fd);
  snprintf(gPut.path, sizeof gPut.path, "%s", argv[0]);
  gPut.mode = (unsigned)strtoul(argv[1], NULL, 8);
  gPut.size = strtol(argv[2], NULL, 10);
  gPut.got = 0;
  gPut.crc = 0;
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.fjtmp", gPut.path);
  gPut.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (gPut.fd < 0) {
    snprintf(msg, sizeof msg, "fjdbg: put: cannot create %s: %s\nfjdbg: eof\n", tmp, strerror(errno));
    say(msg);
  }
}

static void getRange(const char *path, long long off, long long len, int ranged)
{
  char msg[1200];
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    snprintf(msg, sizeof msg, "fjdbg: get: cannot open %s: %s\nfjdbg: eof\n", path, strerror(errno));
    say(msg);
    if (fd >= 0)
      close(fd);
    return;
  }
  long long total = (long long)st.st_size;
  if (off < 0 || off > total)
    off = total;
  long long want = total - off;
  if (len > 0 && len < want)
    want = len;
  if (ranged) {
    snprintf(msg, sizeof msg, "fjdbg: get total %lld\n", total);
    say(msg);
  }
  snprintf(msg, sizeof msg, "fjdbg: get %lld\n", want);
  say(msg);
  static const char digits[] = "0123456789abcdef";
  unsigned char buf[512];
  char line[3 + 2 * sizeof buf + 2];
  uint32_t crc = 0;
  ssize_t n = 0;
  if (off > 0 && lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
    want = 0;
  while (want > 0 && (n = read(fd, buf, want < (long long)sizeof buf ? (size_t)want : sizeof buf)) > 0) {
    want -= n;
    crc = crc32Update(crc, buf, (size_t)n);
    size_t o = 0;
    line[o++] = '@'; line[o++] = 'd'; line[o++] = ' ';
    for (ssize_t i = 0; i < n; i++) {
      line[o++] = digits[buf[i] >> 4];
      line[o++] = digits[buf[i] & 15];
    }
    line[o++] = '\n';
    writeAll(line, o);
  }
  close(fd);
  snprintf(msg, sizeof msg, "fjdbg: get crc %08x\nfjdbg: eof\n", crc);
  say(msg);
}

static void getFile(const char *path)
{
  getRange(path, 0, 0, 0);
}

static void getFileRange(const char *arg)
{
  char *end;
  long long off = strtoll(arg, &end, 10);
  long long len = strtoll(end, &end, 10);
  while (*end == ' ')
    end++;
  getRange(end, off, len, 1);
}

// Built-ins: `@read PATH` streams a text file to the host; @put/@d/@end and @get move any file (see above).
static void builtin(char *line)
{
  if (strncmp(line, "@d ", 3) == 0)
    putData(line + 3);
  else if (strncmp(line, "@put ", 5) == 0)
    putStart(line + 5);
  else if (strncmp(line, "@end ", 5) == 0)
    putFinish(line + 5);
  else if (strncmp(line, "@get ", 5) == 0)
    getFile(line + 5);
  else if (strncmp(line, "@getr ", 6) == 0)
    getFileRange(line + 6);
  else if (strncmp(line, "@read ", 6) == 0) {
    int fd = open(line + 6, O_RDONLY);
    if (fd < 0) {
      say("fjdbg: open failed\n");
      return;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
      writeAll(buf, (size_t)n);
    close(fd);
    say("\nfjdbg: eof\n");
  } else {
    say("fjdbg: unknown builtin\n");
  }
}

// launchd runs this agent as the com.apple.wifid job, so all of that job's Mach services (CoreWiFi's XPC endpoints) point here. Left
// unclaimed, a client's synchronous call never returns: iOS's first-boot data migration hung in WiFiDataMigrator's CoreWiFi call
// (datamigrator blocked on com.apple.private.corewifi-xpc, 2026-10-24), and Settings' Wi-Fi pane would hang the same way. Check them
// all in on a queue of their own (the debug channel never waits on it) and answer every request at once with an empty reply,
// so synchronous callers return (with no data) instead of blocking. The first refusal per service is logged, with the
// caller's pid, to /private/var/ngpu/fjdbg-wifi-refused.log (not to the channel, where it could cut into a transfer).
// The SDK marks these calls unavailable on iOS; launchd jobs use it all the same.
extern xpc_connection_t fjdbg_create_mach_service(const char *name, dispatch_queue_t q, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");
extern pid_t fjdbg_connection_pid(xpc_connection_t c) __asm__("_xpc_connection_get_pid");
#define FJDBG_REFUSED_LOG "/private/var/ngpu/fjdbg-wifi-refused.log"
static const char *const kWifiServices[] = {
  "com.apple.private.corewifi-xpc", "com.apple.private.corewifi.internal-xpc", "com.apple.private.corewifi.readonly-xpc",
  "com.apple.wifi-xpc", "com.apple.wifid.store.cache", "com.apple.wifid.watchdog",
  "com.apple.usernotifications.delegate.com.apple.wifid.usernotification"};
static int gWifiLogged[sizeof kWifiServices / sizeof kWifiServices[0]];

static void refuseWifiServices(void)
{
  dispatch_queue_t q = dispatch_queue_create("fjdbg.wifi-refuse", DISPATCH_QUEUE_SERIAL);
  for (size_t i = 0; i < sizeof kWifiServices / sizeof kWifiServices[0]; i++) {
    xpc_connection_t listener = fjdbg_create_mach_service(kWifiServices[i], q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener)
      continue;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
      if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
        return;
      xpc_connection_t c = (xpc_connection_t)peer;
      if (!gWifiLogged[i]) {   // the handler runs on the serial queue: no lock needed
        gWifiLogged[i] = 1;
        int fd = open(FJDBG_REFUSED_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
          char msg[256];
          int n = snprintf(msg, sizeof msg, "%ld refused %s (first caller pid %d)\n", (long)time(NULL), kWifiServices[i],
                           (int)fjdbg_connection_pid(c));
          if (n > 0)
            (void)!write(fd, msg, (size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1);
          close(fd);
        }
      }
      // Answer every message that wants a reply with an empty one and keep the connection: a cancelled peer reads as
      // XPC_ERROR_CONNECTION_INTERRUPTED, and CoreWiFi clients reconnect and resend at once (SpringBoard spun at 100% CPU).
      xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
        if (event == XPC_ERROR_CONNECTION_INVALID) {
          xpc_release(c);
          return;
        }
        if (xpc_get_type(event) != XPC_TYPE_DICTIONARY)
          return;
        xpc_object_t reply = xpc_dictionary_create_reply(event);
        if (reply) {
          xpc_connection_send_message(c, reply);
          xpc_release(reply);
        }
      });
      xpc_retain(c);   // kept for the client's lifetime; released when the client goes away
      xpc_connection_activate(c);
    });
    xpc_connection_activate(listener);
  }
}

// fajita-wifid=1 (NOTES-wifi.md, "the real wifid"): launchd's only root RunAtLoad+KeepAlive job in /usr/sbin is com.apple.wifid
// (iOS 16 launchd runs jobs from its signed cache, /System/Library/xpc/launchd.plist, so a new plist never loads). This agent keeps
// being started as that job, but splits: a child (its own session, holding FJDBG_LOCK) stays the debug agent, and the job's own
// process execs Apple's wifid, copied into Data by the addon (FJDBG_REAL_WIFID: trusted by cdhash, so the path does not matter).
// The job keeps its pid and launchd's Mach service ports, so the real wifid serves CoreWiFi. When wifid is relaunched (KeepAlive)
// and the debug agent still holds the lock, the new instance only execs wifid.
#define FJDBG_REAL_WIFID "/private/var/ngpu/fjdbg/wifid.real"
#define FJDBG_LOCK "/private/var/ngpu/fjdbg/agent.lock"
static int bootArgOn(const char *name)
{
  char args[1024] = {0};
  size_t n = sizeof args - 1;
  if (sysctlbyname("kern.bootargs", args, &n, NULL, 0))
    return 0;
  const size_t l = strlen(name);
  for (char *p = args; (p = strstr(p, name)); p += l)
    if ((p == args || p[-1] == ' ') && p[l] == '=' && p[l + 1] == '1' && (p[l + 2] == ' ' || p[l + 2] == 0))
      return 1;
  return 0;
}

// fajita-ssh=1 (NOTES-devmode.md): the SSH dev mode. A supervisor process (its own session, holding FJDBG_SSH_LOCK so a relaunched
// agent does not start a second one) runs the add-on's start script (package/ssh/ssh-start: host keys, authorized_keys, then dropbear
// in the foreground) and restarts it when it exits, waiting 2 s, doubling up to 60 s while it keeps dying within 30 s of its start.
// Its output goes to FJDBG_SSH_LOG. The debug channel never waits on it.
#define FJDBG_SSH_SHELL "/private/var/ngpu/ssh/bin/bash"
#define FJDBG_SSH_START "/private/var/ngpu/ssh/libexec/ssh-start"
#define FJDBG_SSH_STATE "/private/var/ssh"
#define FJDBG_SSH_LOCK FJDBG_SSH_STATE "/supervisor.lock"
#define FJDBG_SSH_LOG FJDBG_SSH_STATE "/ssh.log"
static void sshLog(const char *msg)
{
  int fd = open(FJDBG_SSH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0)
    return;
  char line[256];
  int n = snprintf(line, sizeof line, "%ld fjdbg-ssh: %s\n", (long)time(NULL), msg);
  if (n > 0)
    (void)!write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  close(fd);
}

static void startSshSupervisor(void)
{
  if (access(FJDBG_SSH_START, R_OK) != 0 || access(FJDBG_SSH_SHELL, X_OK) != 0)
    return;   // a build without the SSH add-on
  pid_t sup = fork();
  if (sup != 0)
    return;   // the agent goes on (a failed fork just means no SSH)
  setsid();
  if (gDev >= 0)
    close(gDev);
  mkdir(FJDBG_SSH_STATE, 0700);
  int lock = open(FJDBG_SSH_LOCK, O_RDWR | O_CREAT, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
    _exit(0);   // another supervisor is running
  signal(SIGPIPE, SIG_IGN);
  int delay = 2;
  for (unsigned runs = 1;; runs++) {
    int log = open(FJDBG_SSH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (log >= 0) {
      posix_spawn_file_actions_adddup2(&fa, log, STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&fa, log, STDERR_FILENO);
    }
    posix_spawn_file_actions_addclose(&fa, lock);
    char *const args[] = {(char *)"bash", (char *)FJDBG_SSH_START, NULL};
    char *const env[] = {(char *)"PATH=/private/var/ngpu/ssh/bin:/usr/sbin:/usr/bin:/sbin:/bin", (char *)"HOME=/private/var/root",
                         NULL};
    pid_t pid;
    time_t t0 = time(NULL);
    int rc = posix_spawn(&pid, FJDBG_SSH_SHELL, &fa, NULL, args, env);
    posix_spawn_file_actions_destroy(&fa);
    if (log >= 0)
      close(log);
    char msg[160];
    int status = 0;
    if (rc == 0) {
      snprintf(msg, sizeof msg, "run %u: ssh-start pid %d", runs, (int)pid);
      sshLog(msg);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
      snprintf(msg, sizeof msg, "run %u: exited (status 0x%x) after %ld s", runs, status, (long)(time(NULL) - t0));
    } else
      snprintf(msg, sizeof msg, "run %u: cannot spawn %s: %s", runs, FJDBG_SSH_SHELL, strerror(rc));
    sshLog(msg);
    delay = time(NULL) - t0 >= 30 ? 2 : (delay * 2 > 60 ? 60 : delay * 2);
    sleep((unsigned)delay);
  }
}

int main(int argc, char **argv)
{
  (void)argc;
  if (!strcmp(getprogname(), "wifid") && bootArgOn("fajita-wifid") && access(FJDBG_REAL_WIFID, X_OK) == 0) {
    int lock = open(FJDBG_LOCK, O_RDWR | O_CREAT, 0600);
    const int mine = lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0;
    pid_t child = mine ? fork() : -1;
    if (child == 0) {
      setsid();   // out of the job's process group: launchd's restart of wifid does not take the debug agent with it
      goto agent;   // the child keeps the lock for its lifetime
    }
    if (lock >= 0)
      close(lock);   // the child's descriptor keeps the lock
    execv(FJDBG_REAL_WIFID, argv);
    return 1;   // exec failed: launchd retries (the child, if any, still serves the debug channel)
  }
  refuseWifiServices();
agent:
  if (bootArgOn("fajita-ssh"))
    startSshSupervisor();
  for (int tries = 0; gDev < 0 && tries < 600; tries++) {
    gDev = open(FJDBG_DEV, O_RDWR);
    if (gDev < 0)
      sleep(1);   // the node appears once NativeDwc3's poll thread sees the root mount
  }
  if (gDev < 0)
    return 1;
  say("fjdbg-agent ready\n");
  char line[4096];
  size_t len = 0;
  for (;;) {
    char c;
    ssize_t n = read(gDev, &c, 1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0)
      continue;
    if (c == '\r')
      continue;
    if (c != '\n') {
      if (len < sizeof line - 1)
        line[len++] = c;
      continue;
    }
    line[len] = '\0';
    if (len > 0) {
      if (strncmp(line, "fjdbg-hub-", 10) == 0) {   // the hub's exchange marker: echo it, run nothing
        say(line);
        say("\n");
      } else if (strncmp(line, "@t ", 3) == 0) {   // @t SECONDS CMD...: one command with its own time limit
        char *rest = NULL;
        long t = strtol(line + 3, &rest, 10);
        char *argv[64];
        gTimeout = t > 0 ? (int)t : 60;
        if (rest && split(rest, argv, 64) > 0) {
          runCommand(argv);
          gTimeout = 60;
        }   // `@t N` alone: the limit applies to the next command (older agents just ignore this line)
      } else if (line[0] == '@')
        builtin(line);
      else {
        char *argv[64];
        if (split(line, argv, 64) > 0)
          runCommand(argv);
        gTimeout = 60;
      }
    }
    len = 0;
  }
  return 0;
}
