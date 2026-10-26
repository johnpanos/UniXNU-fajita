# Wi-Fi dev mode: SSH into the iOS 16 userland over Wi-Fi

Goal: develop userland on the phone over Wi-Fi with no reflash: `ssh` with the user's key, an interactive shell with a pty,
scp/sftp, port forwarding. Later (part 2) a stay-up build installed so the phone boots it by itself.

## Build, boot, connect

```
scripts/fajita/build-ssh.sh                  # once: fetch + build the SSH userland into .cache/fajita-inputs/ssh
WIFI_HOLD=1 WIFI_EXTRA_ARGS="fajita-wifi-iokit=1 fajita-wifid=1 fajita-ssh=1" \
  scripts/fajita/test/bg.sh start NAME --max 2700 scripts/fajita/test/wifi-test.sh modem link servers wlfw scan assoc dhcp ping
scripts/fajita/test/user-boot.sh fajita-ssh=1   # or: a stay-up build for hand use
scripts/fajita/devmode-ssh.sh find           # the phone's address and the ssh/scp commands
scripts/fajita/devmode-ssh.sh                # login shell; devmode-ssh.sh CMD... runs a command
```

`build.sh` adds the SSH add-on only when the boot arguments contain `fajita-ssh=1` (default off), and builds it with
build-ssh.sh when `.cache/fajita-inputs/ssh` is missing. Rebuild after changing package/ssh: `scripts/fajita/build-ssh.sh`.

## What runs on the phone

| Piece | Source | Why |
| --- | --- | --- |
| dropbear 2026.94 (`dropbear`, `dropbearkey`, `scp`, `dbclient`) | matt.ucc.asn.au, `package/ssh/dropbear-*.{h,patch}` | small, one configure, builds cleanly against the iPhoneOS SDK; libtomcrypt bundled (no OpenSSL) |
| sftp-server from OpenSSH 10.6p1 | cdn.openbsd.org, `--without-openssl` | dropbear has no SFTP server; modern `scp` uses SFTP |
| bash 5.2.37 | ftp.gnu.org, `package/ssh/bash-fajita.patch` | a full interactive shell; readline links iOS's own libncurses.5.4 (in the dyld cache) |
| toybox 0.8.15 | landley.net, macOS miniconfig + vi, diff, expr, tr, more, hexdump, xzcat | ls/cp/grep/sed/tar/vi/... in one binary; awk (system() unavailable on iOS) and stty (linux/tty.h) do not build |
| terminfo | the build host's ncurses database, compiled to the legacy format at build time | iOS ships libncurses but no database |

The tarballs are checked against sha256 pins in build-ssh.sh (first fetched 2026-10-26). Everything is built with clang for
arm64-apple-ios16.0 through scripts/gpu-local and ad-hoc signed with the debug agent's entitlements (platform-application,
no-container). Nothing built is committed.

Layout on the phone:

- `/private/var/ngpu/ssh/` the add-on (written from the boot payload every boot): `bin/` `libexec/ssh-start` `libexec/sftp-server`
  `etc/profile` `etc/bashrc` `etc/authorized_keys` `etc/motd` `share/terminfo`. toybox applet links are made in `bin/` at start.
- `/private/var/ssh/` state on persistent Data, root 0700: `dropbear_{ed25519,ecdsa,rsa}_host_key`, `authorized_keys`,
  `authorized_keys.local` (keys added on the phone, kept), `ssh.log` (supervisor + ssh-start), `dropbear.log` (connections).

Start-up: launchd runs the debug agent as com.apple.wifid; with `fajita-wifid=1` its child keeps the USB channel while the job execs
Apple's wifid. With `fajita-ssh=1` the agent (either path) forks an SSH supervisor (`package/usb-debug-tools/fjdbg-agent.c`,
startSshSupervisor): its own session, a lock (`/private/var/ssh/supervisor.lock`) so a relaunched agent does not start a second
one, runs `bash ssh-start`, restarts it 2 s after it exits (doubling to 60 s while it keeps dying within 30 s).

## Security choices

- Key-only. Password and PAM auth are compiled out (`DROPBEAR_SVR_PASSWORD_AUTH 0`) and `-s -g` are passed as well; iOS's
  `/etc/passwd` still carries the stock root/mobile hash, which therefore can never log in.
- Single-user dropbear (`DROPBEAR_SVR_MULTIUSER 0`): only root, the account it runs as, can log in. Darwin has no `setresgid`, so
  there is no privilege separation (as on macOS builds of dropbear).
- authorized_keys: the builder's `~/.ssh/*.pub` (or `FAJITA_SSH_KEYS`) are baked into the add-on as public keys; a private key in the
  list stops the build. Keys added on the phone go in `/private/var/ssh/authorized_keys.local`.
- Host keys are generated on the phone at first start and never leave it. The host side uses `HostKeyAlias=fajita`, so the key is
  checked whatever address DHCP hands out.
- Port 22, all interfaces: dropbear binds the wildcard address so it is up before DHCP finishes; the interfaces are lo0 and en0 (Wi-Fi)
  (plus the USB network when `usb-net=1`). Three auth tries per connection, 30 s keepalive. Every connection, auth success and failure
  is in `dropbear.log`.
- The SSH server is a root shell on the LAN for whoever holds the key: build with `fajita-ssh=1` only for dev mode.

## STOPPED (safety classifier)

Part 2, the autoboot install, needs the power-on boot to find a dev mode payload that test runs (which flash their payload to
system_a) never pick up, and the reverse. Established: the UniXNU boot manager takes the first volume holding
`\EFI\UniXNU\unixnu.toml` (UniXNULib/Config.c, UniXNUConfigLoad/TryVolume; `PcdUniXNUConfigPath`, `UNIXNU_CONFIG_DIR`), in handle
order, so two payloads on two partitions cannot be told apart by the same firmware. The piece that derived a separate dev mode boot
image from the existing one was stopped by a safety classifier; nothing of it was kept. Open question for the lead/user: how the dev
mode payload should be selected at power-on (for example a UniXNU boot manager option, built from source, that picks the config by
the slot suffix the ABL passes, or keeping the dev payload on system_a and having the test tools restore it).
