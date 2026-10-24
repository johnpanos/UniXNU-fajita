// fsx: tiny filesystem helper for the iOS userland (no ls/rm/mkdir there).  fsx [ls] DIR [DEPTH] | rm PATH | rmr DIR | mk DIR [UID GID] (UID/GID applied to the last component) | chown PATH UID GID | cat FILE
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
static void walk(const char *p, int depth, int max) {
	DIR *d = opendir(p); if (!d) { printf("%s: cannot open\n", p); return; }
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char b[1024]; snprintf(b, sizeof b, "%s/%s", p, e->d_name);
		struct stat st; lstat(b, &st);
		printf("%s%s %lld\n", b, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
		if (S_ISDIR(st.st_mode) && depth < max) walk(b, depth + 1, max);
	}
	closedir(d);
}
static void rmr(const char *p) {
	struct stat st; if (lstat(p, &st)) return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(p); struct dirent *e;
		while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char b[1024]; snprintf(b, sizeof b, "%s/%s", p, e->d_name); rmr(b); }
		if (d) closedir(d);
		printf("rmdir %s -> %d\n", p, rmdir(p));
	} else printf("unlink %s -> %d\n", p, unlink(p));
}
int main(int c, char **v) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (c > 2 && !strcmp(v[1], "rm")) { printf("unlink -> %d\n", unlink(v[2])); return 0; }
	if (c > 2 && !strcmp(v[1], "rmr")) { rmr(v[2]); return 0; }
	if (c > 2 && !strcmp(v[1], "mk")) { char b[1024]; strlcpy(b, v[2], sizeof b); for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0755); *s = '/'; } int r = mkdir(b, 0755); if (c > 4) { chown(b, atoi(v[3]), atoi(v[4])); for (char *s = b + 1; *s; s++) {} } printf("mkdir -> %d\n", r); return 0; }
	if (c > 3 && !strcmp(v[1], "tmpd")) { if (atoi(v[3])) { setgid(atoi(v[3])); setuid(atoi(v[3])); } char b[1024]; snprintf(b, sizeof b, "%s/temp.XXXXXX", v[2]); char *r = mkdtemp(b); printf("mkdtemp %s -> %s errno %d uid %d\n", b, r ? r : "NULL", errno, getuid()); if (r) rmdir(r); return 0; }
	if (c > 4 && !strcmp(v[1], "chown")) { printf("chown -> %d\n", chown(v[2], atoi(v[3]), atoi(v[4]))); return 0; }
	if (c > 2 && !strcmp(v[1], "cat")) { FILE *f = fopen(v[2], "r"); int ch; while (f && (ch = fgetc(f)) != EOF) putchar(ch); return 0; }
	int a = c > 1 && !strcmp(v[1], "ls"); walk(v[1 + a], 0, c > 2 + a ? atoi(v[2 + a]) : 0); return 0;
}
