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
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

static int gDev = -1;

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
    snprintf(msg, sizeof msg, "fjdbg: cannot run %s: %s\n", argv[0], strerror(rc));
    say(msg);
    close(fds[0]);
    return;
  }
  char buf[2048];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0)
    writeAll(buf, (size_t)n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  char tail[64];
  snprintf(tail, sizeof tail, "fjdbg: exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  say(tail);
}

// A built-in: `@read PATH` streams a file to the host; `@write PATH` then raw bytes
// until the connection's next command boundary is not supported (kept simple: @read only).
static void builtin(char *line)
{
  if (strncmp(line, "@read ", 6) == 0) {
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

int main(void)
{
  for (int tries = 0; gDev < 0 && tries < 600; tries++) {
    gDev = open("/dev/fjdbg", O_RDWR);
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
      if (line[0] == '@')
        builtin(line);
      else {
        char *argv[64];
        if (split(line, argv, 64) > 0)
          runCommand(argv);
      }
    }
    len = 0;
  }
  return 0;
}
