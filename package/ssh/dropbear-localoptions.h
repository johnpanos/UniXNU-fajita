/* fajita SSH dev mode (NOTES-devmode.md): dropbear's compile-time options for the iOS 16 phone. Copied to the dropbear
 * build directory as localoptions.h by scripts/fajita/build-ssh.sh. Everything the add-on installs lives under
 * /private/var/ngpu/ssh (rewritten from the boot add-on each boot); state (host keys, authorized_keys, log) under
 * /private/var/ssh (persistent Data, root 0700). */
#ifndef FAJITA_DROPBEAR_LOCALOPTIONS_H
#define FAJITA_DROPBEAR_LOCALOPTIONS_H

#define FAJITA_SSH_ROOT "/private/var/ngpu/ssh"
#define FAJITA_SSH_STATE "/private/var/ssh"

/* Host keys: generated on the phone at first start (ssh-start.sh, dropbearkey), never shipped. */
#define RSA_PRIV_FILENAME FAJITA_SSH_STATE "/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME FAJITA_SSH_STATE "/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME FAJITA_SSH_STATE "/dropbear_ed25519_host_key"
#define DSS_PRIV_FILENAME FAJITA_SSH_STATE "/dropbear_dss_host_key"

/* Key-only: password authentication is compiled out (iOS's passwd still carries the stock root/mobile hash). */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1
#define DROPBEAR_DSS 0
/* Darwin has no setresgid(): the pre-auth process is not privilege-separated (as on macOS builds). */
#define DROPBEAR_SVR_DROP_PRIVS 0
/* Single-user: only the account dropbear runs as (root) can log in, which also lets port forwarding work without privsep. */
#define DROPBEAR_SVR_MULTIUSER 0

/* iOS has no /bin/sh: root logs in to the add-on's bash (dropbear-fajita.patch). */
#define DROPBEAR_LOGIN_SHELL FAJITA_SSH_ROOT "/bin/bash"

#define SFTPSERVER_PATH FAJITA_SSH_ROOT "/libexec/sftp-server"
#define DROPBEAR_PATH_SSH_PROGRAM FAJITA_SSH_ROOT "/bin/dbclient"
#define DROPBEAR_PIDFILE FAJITA_SSH_STATE "/dropbear.pid"
#define MOTD_FILENAME FAJITA_SSH_ROOT "/etc/motd"
#define DEFAULT_PATH FAJITA_SSH_ROOT "/bin:/usr/bin:/bin"
#define DEFAULT_ROOT_PATH FAJITA_SSH_ROOT "/bin:/usr/sbin:/usr/bin:/sbin:/bin"

#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 1
#define DROPBEAR_SVR_REMOTETCPFWD 1
#define DROPBEAR_SVR_AGENTFWD 1
#define DO_HOST_LOOKUP 0
/* Keep a dead Wi-Fi client from holding a session forever: the server probes every 30 s. */
#define DEFAULT_KEEPALIVE 30

#endif
