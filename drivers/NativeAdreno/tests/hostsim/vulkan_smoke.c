/* SPDX-License-Identifier: MIT */
/* Drives the Turnip darwin backend through the Vulkan ICD entry point against the host copy of the kernel stack
 * (host_gpu_service.cpp via host_iokit_shim.cpp). No shader runs; the software CP only retires submissions. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef PFN_vkVoidFunction (*GetProc)(VkInstance, const char *);
typedef unsigned (*Counter)(void);
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #call, r_); return 1; } } while (0)
#define LOAD(var, name, inst) PFN_##name var = (PFN_##name)get(inst, #name); if (!var) { fprintf(stderr, "missing %s\n", #name); return 1; }

int main(int argc, char **argv)
{
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
   GetProc get = (GetProc)dlsym(lib, "vk_icdGetInstanceProcAddr");
   Counter submissions = (Counter)dlsym(RTLD_DEFAULT, "host_gpu_submissions");
   Counter live = (Counter)dlsym(RTLD_DEFAULT, "host_gpu_live_objects");
   if (!get || !submissions || !live) { fprintf(stderr, "missing entry points\n"); return 1; }

   LOAD(createInstance, vkCreateInstance, NULL);
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "smoke", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance instance;
   CHECK(createInstance(&ici, NULL, &instance));

   LOAD(enumerate, vkEnumeratePhysicalDevices, instance);
   uint32_t count = 0;
   CHECK(enumerate(instance, &count, NULL));
   if (count != 1) { fprintf(stderr, "expected one physical device, got %u\n", count); return 1; }
   VkPhysicalDevice phys;
   CHECK(enumerate(instance, &count, &phys));

   LOAD(getProps, vkGetPhysicalDeviceProperties, instance);
   VkPhysicalDeviceProperties props;
   getProps(phys, &props);
   printf("device: %s (vendor 0x%x device 0x%x)\n", props.deviceName, props.vendorID, props.deviceID);

   LOAD(getMemProps, vkGetPhysicalDeviceMemoryProperties, instance);
   VkPhysicalDeviceMemoryProperties mem;
   getMemProps(phys, &mem);
   int hostVisible = -1;
   for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { hostVisible = i; break; }
   }
   printf("memory types: %u, host visible+coherent type: %d\n", mem.memoryTypeCount, hostVisible);
   if (hostVisible < 0) return 1;

   LOAD(getQueueFamilies, vkGetPhysicalDeviceQueueFamilyProperties, instance);
   uint32_t families = 0;
   getQueueFamilies(phys, &families, NULL);
   VkQueueFamilyProperties fam[8];
   if (families > 8) families = 8;
   getQueueFamilies(phys, &families, fam);
   int gfx = -1;
   for (uint32_t i = 0; i < families; i++) if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
   if (gfx < 0) return 1;

   LOAD(createDevice, vkCreateDevice, instance);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = gfx, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice device;
   CHECK(createDevice(phys, &dci, NULL, &device));
   printf("device created, kernel objects live: %u, submissions: %u\n", live(), submissions());

   PFN_vkGetDeviceProcAddr getDeviceProc = (PFN_vkGetDeviceProcAddr)get(instance, "vkGetDeviceProcAddr");
#define DLOAD(var, name) PFN_##name var = (PFN_##name)getDeviceProc(device, #name); if (!var) { fprintf(stderr, "missing %s\n", #name); return 1; }
   DLOAD(getQueue, vkGetDeviceQueue);
   DLOAD(allocMem, vkAllocateMemory);
   DLOAD(mapMem, vkMapMemory);
   DLOAD(unmapMem, vkUnmapMemory);
   DLOAD(freeMem, vkFreeMemory);
   DLOAD(createBuffer, vkCreateBuffer);
   DLOAD(destroyBuffer, vkDestroyBuffer);
   DLOAD(getBufReq, vkGetBufferMemoryRequirements);
   DLOAD(bindBuf, vkBindBufferMemory);
   DLOAD(createPool, vkCreateCommandPool);
   DLOAD(destroyPool, vkDestroyCommandPool);
   DLOAD(allocCmd, vkAllocateCommandBuffers);
   DLOAD(beginCmd, vkBeginCommandBuffer);
   DLOAD(endCmd, vkEndCommandBuffer);
   DLOAD(cmdFill, vkCmdFillBuffer);
   DLOAD(createFence, vkCreateFence);
   DLOAD(destroyFence, vkDestroyFence);
   DLOAD(waitFences, vkWaitForFences);
   DLOAD(submit, vkQueueSubmit);
   DLOAD(waitIdle, vkDeviceWaitIdle);
   DLOAD(destroyDevice, vkDestroyDevice);

   VkQueue queue;
   getQueue(device, gfx, 0, &queue);

   /* Host-visible memory: allocate, map, write, read back through the kernel-provided mapping. */
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1 << 20, .memoryTypeIndex = hostVisible};
   VkDeviceMemory memory;
   CHECK(allocMem(device, &mai, NULL, &memory));
   volatile uint32_t *ptr;
   CHECK(mapMem(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&ptr));
   for (unsigned i = 0; i < 1024; i++) ptr[i] = 0x1000 + i;
   unsigned bad = 0;
   for (unsigned i = 0; i < 1024; i++) bad += ptr[i] != 0x1000 + i;
   printf("mapped 1 MiB through the kernel descriptor, mismatches: %u\n", bad);
   if (bad) return 1;

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1 << 20,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer;
   CHECK(createBuffer(device, &bci, NULL, &buffer));
   VkMemoryRequirements req;
   getBufReq(device, buffer, &req);
   CHECK(bindBuf(device, buffer, memory, 0));

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = gfx};
   VkCommandPool pool;
   CHECK(createPool(device, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   CHECK(allocCmd(device, &cai, &cmd));
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(beginCmd(cmd, &cbi));
   cmdFill(cmd, buffer, 0, 4096, 0xdeadbeef);
   CHECK(endCmd(cmd));

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(createFence(device, &fci, NULL, &fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
   unsigned before = submissions();
   CHECK(submit(queue, 1, &si, fence));
   CHECK(waitFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   printf("fill-buffer submission retired: kernel submissions %u -> %u\n", before, submissions());
   if (submissions() <= before) return 1;

   CHECK(waitIdle(device));
   destroyFence(device, fence, NULL);
   destroyPool(device, pool, NULL);
   destroyBuffer(device, buffer, NULL);
   unmapMem(device, memory);
   freeMem(device, memory, NULL);
   destroyDevice(device, NULL);
   printf("device destroyed, kernel objects live: %u\n", live());
   printf("PASS\n");
   return live() == 0 ? 0 : 2;
}
