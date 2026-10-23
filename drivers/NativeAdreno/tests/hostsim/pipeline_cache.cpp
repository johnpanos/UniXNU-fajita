/* SPDX-License-Identifier: MIT */
/* Real Turnip shader compilation/cache import over the software kernel peer.
 * No command is submitted and no shader is executed. */
#include <vulkan/vulkan.h>
#include "NativeTurnipShaders.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <vector>

#define REQUIRE(x) do {if (!(x)) {std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while (0)
#define OK(x) REQUIRE((x)==VK_SUCCESS)
#define LOAD(name) auto name=(PFN_##name)get(instance,#name);REQUIRE(name)

int main(int argc,char **argv) {
    if(argc!=4)return 2;
    const bool produce=!std::strcmp(argv[3],"produce");
    const bool mismatch=!std::strcmp(argv[3],"uuid-miss");
    const bool shaderMismatch=!std::strcmp(argv[3],"shader-miss");
    REQUIRE(produce||mismatch||shaderMismatch||!std::strcmp(argv[3],"consume"));
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library)std::fprintf(stderr,"dlopen: %s\n",dlerror());
    REQUIRE(library);
    auto get=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");REQUIRE(get);
    auto submissions=(unsigned(*)())dlsym(RTLD_DEFAULT,"host_gpu_submissions");
    auto live=(unsigned(*)())dlsym(RTLD_DEFAULT,"host_gpu_live_objects");REQUIRE(submissions&&live);
    VkInstance instance=VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="native pipeline cache control",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    OK(vkCreateInstance(&ii,nullptr,&instance));
    LOAD(vkEnumeratePhysicalDevices);LOAD(vkGetPhysicalDeviceProperties);LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkCreateDevice);LOAD(vkDestroyDevice);LOAD(vkDestroyInstance);LOAD(vkDeviceWaitIdle);
    LOAD(vkCreateShaderModule);LOAD(vkDestroyShaderModule);
    LOAD(vkCreateDescriptorSetLayout);LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreatePipelineCache);LOAD(vkGetPipelineCacheData);LOAD(vkDestroyPipelineCache);
    LOAD(vkCreateComputePipelines);LOAD(vkDestroyPipeline);
    uint32_t count=1;VkPhysicalDevice physical;OK(vkEnumeratePhysicalDevices(instance,&count,&physical));REQUIRE(count==1);
    VkPhysicalDeviceProperties properties={};vkGetPhysicalDeviceProperties(physical,&properties);
    count=0;OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr));
    std::vector<VkExtensionProperties> listed(count);OK(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,listed.data()));
    const char *extensions[]={VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME};
    for(auto name:extensions){bool found=false;for(auto &e:listed)found|=!std::strcmp(e.extensionName,name);REQUIRE(found);}
    VkPhysicalDevicePipelineCreationCacheControlFeatures control={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&control};
    vkGetPhysicalDeviceFeatures2(physical,&features);REQUIRE(control.pipelineCreationCacheControl);
    VkQueueFamilyProperties families[8];count=8;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    int family=-1;for(uint32_t i=0;i<count;i++)if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=(int)i;break;}REQUIRE(family>=0);
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=(uint32_t)family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&control,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
                           .enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    VkDevice device;OK(vkCreateDevice(physical,&di,nullptr,&device));
    const unsigned submitted=submissions();
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout set;OK(vkCreateDescriptorSetLayout(device,&si,nullptr,&set));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.offset=0,.size=8};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VkPipelineLayout layout;OK(vkCreatePipelineLayout(device,&li,nullptr,&layout));
    std::vector<uint32_t> spirv(std::begin(kSpv_smoke_fill_comp),std::end(kSpv_smoke_fill_comp));
    if(shaderMismatch){
        unsigned changed=0;
        for(size_t i=5;i<spirv.size();){
            unsigned words=spirv[i]>>16;REQUIRE(words&&i+words<=spirv.size());
            // Own fixture: change OpExecutionMode LocalSize from 64x1x1 to 32x1x1.
            if(spirv[i]==0x00060010&&spirv[i+2]==17){REQUIRE(spirv[i+3]==64);spirv[i+3]=32;++changed;}
            i+=words;
        }
        REQUIRE(changed==1);
    }
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=spirv.size()*sizeof(uint32_t),.pCode=spirv.data()};
    VkShaderModule module;OK(vkCreateShaderModule(device,&mi,nullptr,&module));
    std::vector<char> data;
    if(!produce){
        std::ifstream input(argv[2],std::ios::binary);REQUIRE(input);
        data.assign(std::istreambuf_iterator<char>(input),{});REQUIRE(data.size()>32);
        uint32_t words[4];std::memcpy(words,data.data(),sizeof words);
        REQUIRE(words[0]==32&&words[1]==VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
        REQUIRE(words[2]==properties.vendorID&&words[3]==properties.deviceID);
        REQUIRE(!std::memcmp(data.data()+16,properties.pipelineCacheUUID,VK_UUID_SIZE));
        if(mismatch)data[16]^=1;
    }
    VkPipelineCacheCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=data.size(),.pInitialData=data.data()};
    VkPipelineCache cache;OK(vkCreatePipelineCache(device,&ci,nullptr,&cache));
    VkPipelineCreationFeedback whole={},stage={};
    VkPipelineCreationFeedbackCreateInfo feedback={.sType=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pPipelineCreationFeedback=&whole,.pipelineStageCreationFeedbackCount=1,.pPipelineStageCreationFeedbacks=&stage};
    VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.pNext=&feedback,
        .flags=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},
        .layout=layout};
    VkPipeline pipeline=VK_NULL_HANDLE;
    auto began=std::chrono::steady_clock::now();
    VkResult strict=vkCreateComputePipelines(device,cache,1,&pi,nullptr,&pipeline);
    double strictUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count();
    double compileUs=0;
    if(produce||mismatch||shaderMismatch){
        REQUIRE(strict==VK_PIPELINE_COMPILE_REQUIRED&&pipeline==VK_NULL_HANDLE);
        if(produce){
            pi.flags=0;whole={};stage={};began=std::chrono::steady_clock::now();
            OK(vkCreateComputePipelines(device,cache,1,&pi,nullptr,&pipeline));
            compileUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count();
            REQUIRE(whole.flags&VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT);
            REQUIRE(!(whole.flags&VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT));
            size_t bytes=0;OK(vkGetPipelineCacheData(device,cache,&bytes,nullptr));REQUIRE(bytes>32);
            data.resize(bytes);OK(vkGetPipelineCacheData(device,cache,&bytes,data.data()));
            std::ofstream output(argv[2],std::ios::binary);REQUIRE(output);output.write(data.data(),bytes);REQUIRE(output.good());
        }
    }else{
        REQUIRE(strict==VK_SUCCESS&&pipeline!=VK_NULL_HANDLE);
        REQUIRE((whole.flags&(VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT|VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))==
                (VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT|VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT));
    }
    if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
    vkDestroyPipelineCache(device,cache,nullptr);vkDestroyShaderModule(device,module,nullptr);
    vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,set,nullptr);
    OK(vkDeviceWaitIdle(device));REQUIRE(submissions()==submitted);
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);REQUIRE(live()==0);
    char uuid[33]={};for(unsigned i=0;i<16;i++)std::snprintf(uuid+2*i,3,"%02x",properties.pipelineCacheUUID[i]);
    std::printf("PASS: Turnip pipeline cache %s; uuid=%s bytes=%zu strict=%d feedback=0x%x strict_us=%.1f compile_us=%.1f; no shader execution\n",
                argv[3],uuid,data.size(),strict,whole.flags,strictUs,compileUs);
    return 0;
}
