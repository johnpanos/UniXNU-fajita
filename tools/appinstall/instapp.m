// instapp: install an .app directory (PackageType Developer) through MobileInstallation / LaunchServices.
//   instapp [-root] mi PATH | ls PATH | uninstall BUNDLEID | list | launch BUNDLEID
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <unistd.h>
#import <grp.h>
#import <signal.h>
#import <execinfo.h>
static void onalrm(int s) { void *bt[40]; int n = backtrace(bt, 40); backtrace_symbols_fd(bt, n, 1); alarm(20); }
static void cb(CFDictionaryRef st, void *ctx) { printf("status: %s\n", [[(__bridge NSDictionary *)st description] UTF8String]); }
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int asroot = argc > 1 && !strcmp(argv[1], "-root"); if (asroot) { argc--; argv++; }
	if (getuid() == 0 && !asroot) { setenv("HOME", "/var/mobile", 1); gid_t g = 501; setgroups(1, &g); setgid(501); setuid(501); }
	printf("uid %d\n", getuid());
	@autoreleasepool {
		dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		void *mi = dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW);
		signal(SIGALRM, onalrm); alarm(40);
		NSString *cmd = argc > 1 ? @(argv[1]) : @"list";
		Class W = NSClassFromString(@"LSApplicationWorkspace");
		id ws = [W performSelector:@selector(defaultWorkspace)];
		if ([cmd isEqual:@"mi"]) {
			int (*inst)(CFStringRef, CFDictionaryRef, void *, void *) = dlsym(mi, "MobileInstallationInstall");
			printf("MobileInstallationInstall %p\n", inst);
			int r = inst((__bridge CFStringRef)@(argv[2]), (__bridge CFDictionaryRef)@{@"PackageType": @"Developer"}, cb, NULL);
			printf("-> %d\n", r);
		} else if ([cmd isEqual:@"ls"]) {
			NSError *e = nil;
			BOOL ok = ((BOOL (*)(id, SEL, id, id, NSError **))objc_msgSend)(ws, NSSelectorFromString(@"installApplication:withOptions:error:"), [NSURL fileURLWithPath:@(argv[2])], @{@"PackageType": @"Developer"}, &e);
			printf("installApplication -> %d err %s\n", ok, [[e description] UTF8String]);
		} else if ([cmd isEqual:@"uninstall"]) {
			BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, NSSelectorFromString(@"uninstallApplication:withOptions:"), @(argv[2]));
			printf("uninstall -> %d\n", ok);
		} else if ([cmd isEqual:@"launch"]) {
			BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, NSSelectorFromString(@"openApplicationWithBundleID:"), @(argv[2]));
			printf("launch -> %d\n", ok);
		} else {
			for (id p in [ws performSelector:NSSelectorFromString(@"allInstalledApplications")]) printf("%s\n", [[p performSelector:NSSelectorFromString(@"applicationIdentifier")] UTF8String]);
		}
	}
	return 0;
}
