/* SPDX-License-Identifier: MIT */
/* QEMU-only compiler/cache instrument. The unchanged iOS Turnip library talks
 * to the authored software peer, never to a physical GPU. Separate children
 * preserve cold process boundaries; the worker normally links the own peer,
 * and original dyld/AMFI decide loading and interposition. */
#define main pipeline_cache_case_main
#include "pipeline_cache.cpp"
#undef main
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

extern char **environ;
static uint64_t result[48];
alignas(4096) static unsigned char snapshot[65536];
alignas(4096) static unsigned char workerOutput[4096];
static const char *workerLog="/private/var/tmp/turnip-compiler-worker.log";
static const char *driverPath="/System/Library/Extensions/OfflineTurnipCompiler/libvulkan_freedreno.dylib";

static void stop(unsigned failure) {
    result[1]=failure;
    if(FILE *file=fopen(workerLog,"rb")) {
        result[11]=fread(workerOutput,1,sizeof workerOutput-1,file);
        fclose(file);
    }
    __asm__ volatile("mov x13, %0\nmov x14, %1\nmov x15, %2\nbrk #0"::"r"(result),"r"(snapshot),"r"(workerOutput):"x13","x14","x15","memory");
    _exit(99);
}

static void mountCompilerView() {
    char uuid[37]={};FILE *file=fopen("/private/var/tmp/compiler-data-uuid","rb");
    if(!file)stop(60);
    size_t count=fread(uuid,1,37,file);fclose(file);
    if(count!=36 || uuid[36])stop(61);
    CFStringRef identity=CFStringCreateWithCString(nullptr,uuid,kCFStringEncodingASCII);
    CFMutableDictionaryRef match=IOServiceMatching("IOMedia");
    if(!identity || !match)stop(62);
    const void *keys[]={CFSTR("UUID")},*values[]={identity};
    CFDictionaryRef properties=CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!properties)stop(63);
    CFDictionarySetValue(match,CFSTR("IOPropertyMatch"),properties);CFRelease(properties);
    io_iterator_t iterator=0;
    if(IOServiceGetMatchingServices(0,match,&iterator) || !iterator)stop(64);
    char device[64]={};unsigned found=0;
    while(io_service_t media=IOIteratorNext(iterator)) {
        ++found;
        CFTypeRef observed=IORegistryEntryCreateCFProperty(media,CFSTR("UUID"),nullptr,0);
        CFTypeRef name=IORegistryEntryCreateCFProperty(media,CFSTR("BSD Name"),nullptr,0);
        if(!observed || !CFEqual(observed,identity) || !name || CFGetTypeID(name)!=CFStringGetTypeID())stop(65);
        char bsd[56]={};
        if(!CFStringGetCString((CFStringRef)name,bsd,sizeof bsd,kCFStringEncodingASCII) || strncmp(bsd,"disk",4))stop(66);
        for(const char *p=bsd;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')))stop(67);
        bool backed=false;io_registry_entry_t node=media;
        for(unsigned depth=0;node && depth<64;++depth) {
            if(IOObjectConformsTo(node,"NativeRamBlock")){backed=true;break;}
            io_registry_entry_t above=0;auto error=IORegistryEntryGetParentEntry(node,"IOService",&above);
            if(node!=media)IOObjectRelease(node);
            node=error?0:above;
        }
        if(node && node!=media)IOObjectRelease(node);
        if(!backed)stop(68);
        snprintf(device,sizeof device,"/dev/%s",bsd);
        CFRelease(observed);CFRelease(name);IOObjectRelease(media);
    }
    IOObjectRelease(iterator);CFRelease(identity);
    if(found!=1 || !device[0])stop(69);
    const char *point="/System/Library/Extensions",*directory="/private/var/tmp/native-compiler-addons";
    struct stat block={},source={},before={},after={},original={},code={};struct statfs fs={};
    if(stat(device,&block) || !S_ISBLK(block.st_mode) || stat(point,&before) || stat(directory,&source) || source.st_dev!=block.st_rdev)stop(70);
    if(mount("bindfs",point,MNT_RDONLY|MNT_UNION,(void *)directory))stop(71);
    if(statfs(point,&fs) || (fs.f_flags&(MNT_RDONLY|MNT_UNION))!=(MNT_RDONLY|MNT_UNION) ||
       strcmp(fs.f_mntfromname,directory) || strcmp(fs.f_fstypename,"bindfs") || stat(point,&after) ||
       after.st_dev==before.st_dev || stat(driverPath,&code) || code.st_dev!=after.st_dev ||
       stat("/System/Library/Extensions/AGXMetalA10.bundle/Info.plist",&original) || original.st_dev!=before.st_dev)stop(72);
    result[38]=fs.f_flags;result[39]=(uint32_t)before.st_dev;result[40]=(uint32_t)after.st_dev;
    result[41]=(uint32_t)original.st_dev;result[42]=(uint32_t)code.st_dev;result[43]=found;result[44]=1;
}

int main(int argc,char **argv) {
    if(argc==4) {
        // Never emulate a page-size answer in the target OS process.
        if(getpagesize()!=4096 || sysconf(_SC_PAGESIZE)!=4096 || getenv("HOSTSIM_PAGE_SIZE"))return 92;
        if(!strcmp(argv[3],"startup-control"))return 0;
        return pipeline_cache_case_main(argc,argv);
    }
    result[0]=0x54435051494f5331ull;
    result[2]=(uint64_t)getpid();result[3]=(uint64_t)getpagesize();result[4]=(uint64_t)sysconf(_SC_PAGESIZE);
    if(argc!=1 || getpid()!=1 || result[3]!=4096 || result[4]!=4096)stop(1);
    mountCompilerView();
    if(setenv("MESA_SHADER_CACHE_DISABLE","true",1) || unsetenv("DYLD_INSERT_LIBRARIES") || unsetenv("HOSTSIM_PAGE_SIZE"))stop(2);
    if(unlink("/private/var/tmp/turnip-compiler-cache.bin") && errno!=ENOENT)stop(3);
    if(unlink(workerLog) && errno!=ENOENT)stop(5);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions) ||
       posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,workerLog,O_WRONLY|O_CREAT|O_APPEND,0600) ||
       posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO))stop(4);
    const char *modes[]={"produce","consume","uuid-miss","shader-miss"};
    for(unsigned i=0;i<4;++i) {
        char *arguments[]={const_cast<char *>("compiler-worker"),const_cast<char *>(driverPath),
            const_cast<char *>("/private/var/tmp/turnip-compiler-cache.bin"),const_cast<char *>(modes[i]),nullptr};
        pid_t child=0;
        int error=posix_spawn(&child,"/private/var/tmp/compiler-worker",&actions,nullptr,arguments,environ);
        result[12+3*i]=(uint64_t)child;result[13+3*i]=(uint64_t)error;
        if(error || child<=1)stop(10+i);
        int status=0;
        while(waitpid(child,&status,0)<0)if(errno!=EINTR)stop(20+i);
        result[14+3*i]=(uint64_t)status;
        if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)stop(30+i);
        result[5]=i+1;
    }
    posix_spawn_file_actions_destroy(&actions);
    FILE *file=fopen("/private/var/tmp/turnip-compiler-cache.bin","rb");
    if(!file)stop(40);
    const size_t bytes=fread(snapshot,1,sizeof(snapshot),file);
    const bool ok=!ferror(file) && bytes>32 && bytes<sizeof(snapshot) && feof(file);
    fclose(file);
    if(!ok)stop(41);
    result[6]=bytes;
    uint32_t header[4];memcpy(header,snapshot,sizeof header);
    result[7]=header[0];result[8]=header[1];result[9]=header[2];result[10]=header[3];
    memcpy(result+24,snapshot+16,VK_UUID_SIZE);
    stop(0);
}
