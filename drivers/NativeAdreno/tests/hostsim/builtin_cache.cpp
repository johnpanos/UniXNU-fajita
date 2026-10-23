/* SPDX-License-Identifier: MIT */
/* Actual Turnip builtin export/embedding and lifetime checks. The mandatory
 * software peer executes no shader. No hardware timing claim follows. */
#include <vulkan/vulkan.h>
#include "util/blake3/blake3.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <unistd.h>
#include <TargetConditionals.h>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"FAIL builtin line %d: %s\n",__LINE__,#x);return 1;}} while(0)
#define LOAD(name) auto name=reinterpret_cast<PFN_##name>(get(instance,#name));REQUIRE(name)
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    bool produce=!std::strcmp(argv[3],"produce"),consume=!std::strcmp(argv[3],"consume"),refuse=!std::strcmp(argv[3],"refuse");
    REQUIRE(produce||consume||refuse);
#if TARGET_OS_IPHONE
    REQUIRE(getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && !getenv("HOSTSIM_PAGE_SIZE"));
#endif
    auto live=reinterpret_cast<unsigned(*)()>(dlsym(RTLD_DEFAULT,"host_gpu_live_objects"));
    auto submitted=reinterpret_cast<unsigned(*)()>(dlsym(RTLD_DEFAULT,"host_gpu_submissions"));REQUIRE(live&&submitted);
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library)std::fprintf(stderr,"%s\n",dlerror());REQUIRE(library);
    auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vk_icdGetInstanceProcAddr"));REQUIRE(get);
    auto exportPack=reinterpret_cast<VkResult(*)(VkDevice,size_t *,void *)>(dlsym(library,"tu_export_builtin_shader_pack"));
    auto validate=reinterpret_cast<VkResult(*)(VkDevice,size_t,const void *)>(dlsym(library,"tu_validate_builtin_shader_pack"));REQUIRE(exportPack&&validate);
    VkInstance instance=VK_NULL_HANDLE;LOAD(vkCreateInstance);
    VkApplicationInfo app={};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii={};ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ii.pApplicationInfo=&app;
    REQUIRE(vkCreateInstance(&ii,nullptr,&instance)==VK_SUCCESS);
    LOAD(vkDestroyInstance);LOAD(vkEnumeratePhysicalDevices);LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);LOAD(vkDestroyDevice);LOAD(vkDeviceWaitIdle);
    uint32_t n=1;VkPhysicalDevice physical=VK_NULL_HANDLE;REQUIRE(vkEnumeratePhysicalDevices(instance,&n,&physical)==VK_SUCCESS&&n==1);
    VkQueueFamilyProperties families[8]={};n=8;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families);
    REQUIRE(n>0&&n<=8);int family=-1;for(unsigned i=0;i<n;++i)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=(int)i;break;}REQUIRE(family>=0);
    float priority=1;VkDeviceQueueCreateInfo qi={};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di={};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkDevice device=VK_NULL_HANDLE;auto begin=std::chrono::steady_clock::now();const VkResult created=vkCreateDevice(physical,&di,nullptr,&device);
    const double elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
    size_t bytes=0;
    if(refuse)REQUIRE(created==VK_ERROR_INITIALIZATION_FAILED&&device==VK_NULL_HANDLE);
    else {
        REQUIRE(created==VK_SUCCESS&&device);REQUIRE(exportPack(device,&bytes,nullptr)==VK_SUCCESS&&bytes>128&&bytes<4*1024*1024);
        std::vector<unsigned char> record(bytes);REQUIRE(exportPack(device,&bytes,record.data())==VK_SUCCESS&&validate(device,bytes,record.data())==VK_SUCCESS);
        unsigned char guard[16];std::memset(guard,0xa5,sizeof(guard));size_t shortSize=sizeof(guard);
        REQUIRE(exportPack(device,&shortSize,guard)==VK_INCOMPLETE&&shortSize==bytes&&std::all_of(std::begin(guard),std::end(guard),[](auto x){return x==0xa5;}));
        auto bad=record;bad[bytes-1]^=1;
        REQUIRE(validate(device,bytes,bad.data())==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(validate(device,bytes-1,record.data())==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(validate(device,0,nullptr)==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(validate(device,4*1024*1024+1,reinterpret_cast<void *>(1))==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(record.size()>256&&!std::memcmp(record.data(),"TUBUILT1",8));
        auto reseal=[](std::vector<unsigned char> &value){blake3_hasher h;blake3_hasher_init(&h);blake3_hasher_update(&h,value.data(),value.size()-32);blake3_hasher_finalize(&h,value.data()+value.size()-32,32);};
        auto changed=[&](size_t at,uint32_t value){auto mutated=record;REQUIRE(at+4<=mutated.size()-32);std::memcpy(mutated.data()+at,&value,4);reseal(mutated);REQUIRE(validate(device,mutated.size(),mutated.data())==VK_ERROR_INITIALIZATION_FAILED);return 0;};
        REQUIRE(changed(16,0)==0); // exact source/profile identity, valid checksum
        REQUIRE(changed(80,0)==0); // actual GPU identity
        REQUIRE(changed(12,16)==0); // incomplete variant set
        REQUIRE(changed(144,1)==0); // duplicated/reordered entry
        REQUIRE(changed(148,0x10000000)==0); // entry allocation bound
        uint32_t keyBytes=0;std::memcpy(&keyBytes,record.data()+116,4);REQUIRE(keyBytes<1024);
        REQUIRE(changed(152+keyBytes+8,0xffffffff)==0); // unchecked IR3 allocation in the old generic reader
        bad=record;bad.insert(bad.end()-32,0x7f);reseal(bad);REQUIRE(validate(device,bad.size(),bad.data())==VK_ERROR_INITIALIZATION_FAILED);
        if(produce){std::ofstream output(argv[2],std::ios::binary);REQUIRE(output);output.write(reinterpret_cast<char *>(record.data()),record.size());REQUIRE(output.good());}
        else {std::ifstream input(argv[2],std::ios::binary);REQUIRE(input);std::vector<unsigned char> previous((std::istreambuf_iterator<char>(input)),{});REQUIRE(record==previous);}
        REQUIRE(vkDeviceWaitIdle(device)==VK_SUCCESS);vkDestroyDevice(device,nullptr);
    }
    vkDestroyInstance(instance,nullptr);REQUIRE(live()==0&&submitted()==0);
    std::printf("PASS: builtin cache %s pid=%d variants=17 bytes=%zu device_us=%.1f negatives=%u live=0 submissions=0; software peer, no shader execution\n",argv[3],getpid(),bytes,elapsed,refuse?0:11);
}
