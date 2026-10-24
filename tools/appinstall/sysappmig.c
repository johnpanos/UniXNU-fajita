// sysappmig: run only iOS's SystemAppMigrator data-migration plugin (installs the staged deletable system apps)
// without running the rest of DataMigrator. Usage: sysappmig [probe]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <signal.h>
#include <sys/types.h>
#include <execinfo.h>
#include <unistd.h>
#include <grp.h>
#include <sys/ucontext.h>

static void pr(void *pc) {
	Dl_info di; memset(&di, 0, sizeof di); dladdr(pc, &di);
	printf("  %p base %p off 0x%lx %s %s\n", pc, di.dli_fbase, (long)((char *)pc - (char *)di.dli_fbase),
	    di.dli_fname ? strrchr(di.dli_fname, '/') : "?", di.dli_sname ? di.dli_sname : "?");
}
static void onsig(int sig, siginfo_t *si, void *ctx) {
	ucontext_t *uc = ctx;
	printf("signal %d addr %p\n", sig, si->si_addr);
	{ Dl_info di; memset(&di, 0, sizeof di); dladdr((void *)uc->uc_mcontext->__ss.__pc, &di);
	  if (di.dli_sname && strstr(di.dli_sname, "abort_with_payload") && uc->uc_mcontext->__ss.__x[4])
		printf("abort reason: %s\n", (const char *)uc->uc_mcontext->__ss.__x[4]); }
	pr((void *)uc->uc_mcontext->__ss.__pc); pr((void *)uc->uc_mcontext->__ss.__lr);
	void **fp = (void **)uc->uc_mcontext->__ss.__fp;
	for (int i = 0; fp && i < 40; i++) { pr(fp[1]); fp = (void **)fp[0]; }
	if (sig == SIGALRM) { alarm(20); return; }
	_exit(100 + sig);
}
static const char *cdesc(id o);
static void onexc(id e) {
	printf("uncaught exception: %s\n", cdesc(e));
	printf("stack: %s\n", cdesc(((id (*)(id, SEL))objc_msgSend)(e, sel_registerName("callStackSymbols"))));
	_exit(99);
}

typedef id (*msg0)(id, SEL);
typedef id (*msg1)(id, SEL, id);
typedef signed char (*msgB)(id, SEL);
#define M0(o, s) ((msg0)objc_msgSend)((id)(o), sel_registerName(s))
#define M1(o, s, a) ((msg1)objc_msgSend)((id)(o), sel_registerName(s), (id)(a))

static id str(const char *s) {
	typedef id (*f)(id, SEL, const char *);
	return ((f)objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), s);
}
static const char *cdesc(id o) {
	if (!o) return "(nil)";
	return ((const char *(*)(id, SEL))objc_msgSend)(M0(o, "description"), sel_registerName("UTF8String"));
}


/* the daemon-container lookup MIDaemonConfiguration makes (_UserDataDirectoryBase), with the error spelled out */
static void cq(const char *ident, long cls, long uid, int persona, unsigned long flags) {
	void *(*qcreate)(void) = dlsym(RTLD_DEFAULT, "container_query_create");
	void (*setclass)(void *, long) = dlsym(RTLD_DEFAULT, "container_query_set_class");
	void (*setids)(void *, void *) = dlsym(RTLD_DEFAULT, "container_query_set_identifiers");
	void (*setpers)(void *, const char *) = dlsym(RTLD_DEFAULT, "container_query_set_persona_unique_string");
	void (*setuid_)(void *, long) = dlsym(RTLD_DEFAULT, "container_query_set_uid");
	void (*setflags)(void *, unsigned long) = dlsym(RTLD_DEFAULT, "container_query_operation_set_flags");
	void *(*single)(void *) = dlsym(RTLD_DEFAULT, "container_query_get_single_result");
	void *(*lasterr)(void *) = dlsym(RTLD_DEFAULT, "container_query_get_last_error");
	char *(*errdesc)(void *) = dlsym(RTLD_DEFAULT, "container_error_copy_unlocalized_description");
	const char *(*getpath)(void *) = dlsym(RTLD_DEFAULT, "container_get_path");
	void *(*xstr)(const char *) = dlsym(RTLD_DEFAULT, "xpc_string_create");
	void *(*xarr)(void **, size_t) = dlsym(RTLD_DEFAULT, "xpc_array_create");
	const char **pp = dlsym(RTLD_DEFAULT, "CONTAINER_PERSONA_PRIMARY");
	printf("cq %s class %ld uid %ld persona %d flags 0x%lx: syms %d%d%d%d%d%d%d%d%d%d pp %s\n", ident, cls, uid, persona, flags,
	    !!qcreate, !!setclass, !!setids, !!setpers, !!setuid_, !!setflags, !!single, !!lasterr, !!errdesc, !!getpath, pp ? *pp : "(none)");
	if (!qcreate || !setclass || !setids || !single) return;
	void *q = qcreate(); setclass(q, cls);
	void *s1 = xstr(ident); setids(q, xarr(&s1, 1));
	if (persona && setpers && pp) setpers(q, *pp);
	if (uid >= 0 && setuid_) setuid_(q, uid);
	if (setflags) setflags(q, flags);
	void *r = single(q);
	if (r) printf("  result %p path %s\n", r, getpath ? getpath(r) : "?");
	else { void *e = lasterr ? lasterr(q) : 0; printf("  no result, error %s\n", e && errdesc ? errdesc(e) : "?"); }
}
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	/* DataMigrator runs its plugins as mobile (_RoleAccount mobile in com.apple.migrationpluginwrapper.xpc): MobileInstallation's
	   MIDaemonConfiguration looks up installd's daemon container for the calling user and aborts if that lookup fails. */
	int asroot = argc > 1 && !strcmp(argv[1], "-root");
	if (asroot) { argc--; argv++; }
	if (getuid() == 0 && !asroot) {
		setenv("HOME", "/var/mobile", 1); setenv("USER", "mobile", 1); setenv("LOGNAME", "mobile", 1);
		gid_t g = 501; setgroups(1, &g);
		if (setgid(501) || setuid(501)) { printf("cannot become mobile\n"); return 5; }
	}
	printf("uid %d\n", getuid());
	const char *fw[] = {"/System/Library/Frameworks/Foundation.framework/Foundation",
	    "/System/Library/PrivateFrameworks/DataMigration.framework/DataMigration", NULL};
	for (int i = 0; fw[i]; i++)
		if (!dlopen(fw[i], RTLD_NOW)) { printf("dlopen %s: %s\n", fw[i], dlerror()); return 2; }
	static char altstk[65536]; stack_t ss = { .ss_sp = altstk, .ss_size = sizeof altstk, .ss_flags = 0 }; sigaltstack(&ss, NULL);
	int sigs[] = {SIGALRM, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP, SIGFPE, SIGSYS, 0};
	for (int i = 0; sigs[i]; i++) { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onsig; sa.sa_flags = SA_ONSTACK | SA_SIGINFO; sigaction(sigs[i], &sa, NULL); }
	void (*setexc)(void (*)(id)) = dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
	if (setexc) setexc(onexc);
	id pool = M0(M0(objc_getClass("NSAutoreleasePool"), "alloc"), "init");
	if (argc > 1 && !strcmp(argv[1], "cq")) {
		const char *id = argc > 2 ? argv[2] : "com.apple.installd";
		cq(id, 10, -1, 1, 0x900000001UL);
		cq(id, 10, -1, 0, 0x900000001UL);
		cq(id, 10, -1, 1, 1);
		cq(id, 10, 33, 1, 0x900000001UL);
		cq(id, 10, 0, 1, 0x900000001UL);
		cq("com.apple.mobilemail", 2, -1, 1, 1);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "check")) {
		dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		for (int i = 2; i < argc; i++) {
			id px = M1(objc_getClass("LSApplicationProxy"), "applicationProxyForIdentifier:", str(argv[i]));
			signed char inst = px ? ((msgB)objc_msgSend)(px, sel_registerName("isInstalled")) : 0;
			signed char ph = px ? ((msgB)objc_msgSend)(px, sel_registerName("isPlaceholder")) : 0;
			printf("%s installed=%d placeholder=%d url=%s\n", argv[i], inst, ph, cdesc(px ? M0(px, "bundleURL") : (id)0));
		}
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "launch")) {
		dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		id ws = M0(objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");
		signed char ok = ((signed char (*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("openApplicationWithBundleID:"), str(argv[2]));
		printf("openApplicationWithBundleID %s -> %d\n", argv[2], ok);
		return ok ? 0 : 1;
	}
	if (argc > 2 && !strcmp(argv[1], "kill")) { printf("kill -> %d\n", kill(atoi(argv[2]), SIGKILL)); return 0; }
	if (argc > 1 && !strcmp(argv[1], "bg")) {
		if (fork()) { printf("started in background, log /var/mobile/sysappmig.log\n"); return 0; }
		setsid(); freopen("/var/mobile/sysappmig.log", "w", stdout); setvbuf(stdout, NULL, _IONBF, 0); dup2(1, 2);
	}
	id b = M1(objc_getClass("NSBundle"), "bundleWithPath:",
	    str("/System/Library/DataClassMigrators/SystemAppMigrator.migrator"));
	if (!b || !((msgB)objc_msgSend)(b, sel_registerName("load"))) { printf("bundle load failed\n"); return 3; }
	Class c = (Class)M0(b, "principalClass");
	printf("principal class %s\n", c ? class_getName(c) : "(none)");
	if (!c) return 4;
	printf("calling alloc\n");
	id m = M0(c, "alloc");
	printf("alloc ok %p\n", m);
	m = M0(m, "init");
	printf("init ok %p\n", m);
	printf("migrator %s\n", cdesc(m));
	if (argc > 1 && !strcmp(argv[1], "probe")) {
		printf("itemsToInstall %s\n", cdesc(M0(m, "itemsToInstall")));
		return 0;
	}
	alarm(15); printf("calling performMigration\n");
	signed char ok = ((msgB)objc_msgSend)(m, sel_registerName("performMigration"));
	printf("performMigration -> %d\n", ok);
	printf("installed %s\n", cdesc(M0(m, "appBundleIDsInstalledSuccessfully")));
	M0(pool, "drain");
	return ok ? 0 : 1;
}
