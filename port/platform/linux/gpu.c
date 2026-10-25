/* Linux exact GS: shared HLSL rasterizer, ordered compute batches over 4 MB VRAM.
   Host-visible coherent buffers keep the initial implementation explicit: a fence
   completes each batch before mapped storage is reused. CPU transfers merge only
   dirty pages, preserving unrelated uploads and texture feedback snapshots.
   Native/upscaled rendering remains a separate backend. */
#include "gpu.h"
#include "../../gs/gpu.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_spirv.h"

#include "../../gs/raster_types.h"
#include "../../gs/raster_math.h"

typedef struct {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
} Buffer;
typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkDescriptorSetLayout layout;
    VkDescriptorPool descriptors;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    Buffer buffers[5]; /* b0, u0, t0, t1, t2, with explicit DXC binding shifts */
    Primitive pending[MAX_DRAWS];
    Constants state;
    uint32_t *bin_data;
    unsigned pending_count, memberships, batch_limit, state_context;
    int dirty, texture_valid, batch_safe;
    uint64_t uploaded, state_version, texture_key, clamp_key, tex1_key, mip_key[2];
    uint32_t cpu_dirty[16], texture_dirty[16], texture_pages[16];
    uint32_t *staged;
} Gpu;
static int check(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS) return 1;
    fprintf(stderr, "gs: Vulkan %s failed (%d)\n", operation, result);
    return 0;
}
static void require(VkResult result, const char *operation)
{
    if (!check(result, operation)) exit(1);
}

static int create_buffer(Gpu *p, unsigned index, VkDeviceSize size)
{
    Buffer *b = &p->buffers[index];
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = index ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!check(vkCreateBuffer(p->device, &info, NULL, &b->handle), "create buffer")) return 0;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(p->device, b->handle, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(p->physical, &properties);
    unsigned type = properties.memoryTypeCount;
    /* Mapped VRAM is also read by scanout/snapshot copies. Prefer cached system
       memory: reading an uncached device-local BAR through memcpy is very slow. */
    for (unsigned pass = 0; pass < 2 && type == properties.memoryTypeCount; pass++)
        for (unsigned i = 0; i < properties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if (!pass) required |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & required) == required) { type = i; break; }
        }
    if (type == properties.memoryTypeCount) {
        fprintf(stderr, "gs: Vulkan needs host-visible coherent buffer memory\n");
        return 0;
    }
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (!check(vkAllocateMemory(p->device, &allocation, NULL, &b->memory), "allocate buffer") ||
        !check(vkBindBufferMemory(p->device, b->handle, b->memory, 0), "bind buffer") ||
        !check(vkMapMemory(p->device, b->memory, 0, size, 0, &b->mapped), "map buffer")) return 0;
    memset(b->mapped, 0, (size_t)size);
    return 1;
}

/* Same ordered 8x8 tile lists as the D3D11 reference; packed writes use shader
   atomics (exclusive-word optimization is deliberately disabled in this build). */
static void flush(Gpu *p)
{
    if (!p->pending_count) return;
    unsigned x0 = 2048, y0 = 2048, x1 = 0, y1 = 0;
    for (unsigned i = 0; i < p->pending_count; i++) {
        const uint32_t *b = p->pending[i].bounds;
        if (b[0]/8 < x0) x0 = b[0]/8;
        if (b[1]/8 < y0) y0 = b[1]/8;
        if ((b[0]+b[2]-1)/8 > x1) x1 = (b[0]+b[2]-1)/8;
        if ((b[1]+b[3]-1)/8 > y1) y1 = (b[1]+b[3]-1)/8;
    }
    unsigned width = x1-x0+1, height = y1-y0+1, tiles = width*height;
    uint32_t *bins = p->bin_data;
    memset(bins, 0, tiles*8);
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < p->pending_count; i++) {
            const uint32_t *b = p->pending[i].bounds;
            for (unsigned y = b[1]/8; y <= (b[1]+b[3]-1)/8; y++)
                for (unsigned x = b[0]/8; x <= (b[0]+b[2]-1)/8; x++) {
                    unsigned tile = (y-y0)*width+x-x0;
                    if (pass) bins[bins[tile*2]+bins[tile*2+1]] = i;
                    bins[tile*2+1]++;
                }
        }
        if (!pass) {
            unsigned offset = tiles*2;
            for (unsigned i = 0; i < tiles; i++) {
                bins[i*2] = offset; offset += bins[i*2+1]; bins[i*2+1] = 0;
            }
        }
    }
    p->state.bounds[0] = x0*8; p->state.bounds[1] = y0*8;
    p->state.bounds[2] = width*8; p->state.bounds[3] = height*8;
    p->state.info[0] = width; p->state.info[1] = 0;
    memcpy(p->buffers[0].mapped, &p->state, sizeof p->state);
    memcpy(p->buffers[3].mapped, p->pending, p->pending_count*sizeof(Primitive));
    require(vkResetCommandPool(p->device, p->pool, 0), "reset commands");
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    require(vkBeginCommandBuffer(p->command, &begin), "begin commands");
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(p->command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdBindPipeline(p->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(p->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline_layout, 0, 1, &p->set, 0, NULL);
    vkCmdDispatch(p->command, width, height, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(p->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    require(vkEndCommandBuffer(p->command), "end commands");
    require(vkResetFences(p->device, 1, &p->fence), "reset fence");
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p->command};
    require(vkQueueSubmit(p->queue, 1, &submit, p->fence), "submit compute");
    require(vkWaitForFences(p->device, 1, &p->fence, VK_TRUE, UINT64_MAX), "wait compute");
    p->pending_count = p->memberships = 0;
}
static void flush_draw(Gs *g) { flush(g->backend); }

static void sync_vram(Gs *g)
{
    Gpu *p = g->backend;
    if (!p->dirty) return;
    flush(p);
    for (unsigned page = 0; page < 512; page++)
        if (p->cpu_dirty[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)g->vram+page*8192, (uint8_t *)p->buffers[1].mapped+page*8192, 8192);
    p->dirty = 0;
    memset(p->cpu_dirty, 0, sizeof p->cpu_dirty);
}
static void sync_range(Gs *g, unsigned address, unsigned bytes)
{
    (void)address; (void)bytes;
    sync_vram(g);
}

static void stage_pages(Gs *g, const uint32_t want[16], uint32_t staged[16])
{
    Gpu *p = g->backend;
    flush(p);
    for (unsigned i = 0; i < 16; i++) staged[i] = want[i] & p->cpu_dirty[i];
    for (unsigned page = 0; page < 512; page++)
        if (staged[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)p->staged+page*8192, (uint8_t *)p->buffers[1].mapped+page*8192, 8192);
}
static void fetch_pages(Gs *g, const uint32_t staged[16], uint32_t *image)
{
    Gpu *p = g->backend;
    for (unsigned page = 0; page < 512; page++)
        if (staged[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)image+page*8192, (uint8_t *)p->staged+page*8192, 8192);
}
static void upload_pages(Gs *g)
{
    Gpu *p = g->backend;
    if (p->uploaded == g->pixels_written) return;
    flush(p);
    unsigned any = 0;
    for (unsigned i = 0; i < 16; i++) any |= g->host_dirty[i];
    if (p->uploaded == UINT64_MAX || !any) memset(g->host_dirty, 255, sizeof g->host_dirty);
    for (unsigned page = 0; page < 512; page++)
        if (g->host_dirty[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)p->buffers[1].mapped+page*8192, (uint8_t *)g->vram+page*8192, 8192);
    for (unsigned i = 0; i < 16; i++) p->texture_dirty[i] |= g->host_dirty[i];
    memset(g->host_dirty, 0, sizeof g->host_dirty);
    p->uploaded = g->pixels_written;
}
static void texture_pages(Gpu *gpu, Gs *g, unsigned ctxt)
{
    uint64_t tex = g->reg[GS_TEX0_1 + ctxt], clamp = g->reg[GS_CLAMP_1 + ctxt];
    uint64_t tex1 = g->reg[GS_TEX1_1 + ctxt];
    uint64_t mip[2] = {g->reg[GS_MIPTBP1_1 + ctxt], g->reg[GS_MIPTBP2_1 + ctxt]};
    if (gpu->texture_valid && gpu->texture_key == tex && gpu->clamp_key == clamp &&
        gpu->tex1_key == tex1 && gpu->mip_key[0] == mip[0] && gpu->mip_key[1] == mip[1]) return;
    gs_raster_texture_pages(gpu->texture_pages, g, ctxt);
    gpu->texture_key = tex; gpu->clamp_key = clamp; gpu->texture_valid = 1;
    gpu->tex1_key = tex1; gpu->mip_key[0] = mip[0]; gpu->mip_key[1] = mip[1];
}

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }


static void draw(Gs *g, unsigned type, const GsVertex *v, unsigned count)
{
    Gpu *gpu = g->backend;
    Primitive c = {0};
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    gs_raster_depth(v, type, count, c.depth);
    uint64_t offset = g->reg[GS_XYOFFSET_1 + ctxt], scissor = g->reg[GS_SCISSOR_1 + ctxt];
    int minx = 65536, miny = 65536, maxx = -65536, maxy = -65536;
    for (unsigned i = 0; i < count; i++) {
        int x = (int)v[i].x - (int)(offset & 65535), y = (int)v[i].y - (int)(offset >> 32 & 65535);
        minx = minimum(minx, x); miny = minimum(miny, y); maxx = maximum(maxx, x); maxy = maximum(maxy, y);
        c.position[i][0] = (uint32_t)x; c.position[i][1] = (uint32_t)y;
        c.position[i][2] = v[i].z; c.position[i][3] = v[i].rgba;
        c.uv[i][0] = v[i].u; c.uv[i][1] = v[i].v; c.uv[i][2] = v[i].fog;
        c.stq[i][0] = v[i].s; c.stq[i][1] = v[i].t; c.stq[i][2] = v[i].q;
    }
    minx = maximum(minx >> 4, (int)(scissor & 2047));
    miny = maximum(miny >> 4, (int)(scissor >> 32 & 2047));
    maxx = minimum((maxx + 15) >> 4, (int)(scissor >> 16 & 2047));
    maxy = minimum((maxy + 15) >> 4, (int)(scissor >> 48 & 2047));
    if (minx > maxx || miny > maxy) return;
    if (gpu->state_version != g->state_version || gpu->state_context != ctxt) {
        flush(gpu);
        static const unsigned registers[] = {6,7,8,9,20,21,34,52,53,54,55,59,61,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79};
        for (unsigned r = 0; r < sizeof registers / sizeof *registers; r++) {
            unsigned i = registers[r];
            gpu->state.reg[i][0] = (uint32_t)g->reg[i]; gpu->state.reg[i][1] = (uint32_t)(g->reg[i] >> 32);
        }
        for (unsigned i = 0; i < 256; i++) gpu->state.palette[i] = gs_palette(g, g->reg[GS_TEX0_1 + ctxt], i);
        gpu->state_version = g->state_version; gpu->state_context = ctxt;
        gpu->batch_safe = gs_raster_independent_pixels(g, ctxt);
    }
    c.bounds[0] = (uint32_t)minx; c.bounds[1] = (uint32_t)miny;
    c.bounds[2] = (uint32_t)(maxx - minx + 1); c.bounds[3] = (uint32_t)(maxy - miny + 1);
    c.info[0] = type; c.info[1] = prim; c.info[2] = count;
    unsigned memberships = ((unsigned)maxx / 8 - (unsigned)minx / 8 + 1) *
                           ((unsigned)maxy / 8 - (unsigned)miny / 8 + 1);
    if (gpu->pending_count == gpu->batch_limit || gpu->memberships + memberships + 2 * 65536 > MAX_BIN_WORDS)
        flush(gpu);
    upload_pages(g);
    if (prim & 16) {
        texture_pages(gpu, g, ctxt);
        for (unsigned i = 0; i < 16; i++) if (gpu->texture_pages[i] & gpu->texture_dirty[i]) {
            flush(gpu);
            memcpy(gpu->buffers[2].mapped, gpu->buffers[1].mapped, GS_VRAM_SIZE);
            memset(gpu->texture_dirty, 0, sizeof gpu->texture_dirty);
            break;
        }
    }
    Primitive *p = &gpu->pending[gpu->pending_count++];
    memcpy(p->position, c.position, sizeof p->position); memcpy(p->uv, c.uv, sizeof p->uv);
    memcpy(p->stq, c.stq, sizeof p->stq); memcpy(p->info, c.info, sizeof p->info);
    memcpy(p->bounds, c.bounds, sizeof p->bounds);
    memcpy(p->depth, c.depth, sizeof p->depth);
    gpu->memberships += memberships;
    gpu->dirty = 1;
    uint32_t written[16] = {0};
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], zbuf = g->reg[GS_ZBUF_1 + ctxt];
    unsigned bw = (unsigned)(frame >> 16) & 63;
    gs_mark_pages(written, (unsigned)(frame >> 24) & 63, ((unsigned)frame & 511) * 32, bw, minx,miny,maxx,maxy);
    if (!(zbuf >> 32 & 1))
        gs_mark_pages(written, 48 | ((unsigned)(zbuf >> 24) & 15), ((unsigned)zbuf & 511) * 32, bw, minx,miny,maxx,maxy);
    for (unsigned i = 0; i < 16; i++) { gpu->cpu_dirty[i] |= written[i]; gpu->texture_dirty[i] |= written[i]; }
    if (!gpu->batch_safe) flush(gpu);
}


void gs_gpu_destroy(Gs *g)
{
    Gpu *p = g->backend;
    if (!p) return;
    if (p->device) {
        vkDeviceWaitIdle(p->device);
        if (p->pipeline) vkDestroyPipeline(p->device, p->pipeline, NULL);
        if (p->pipeline_layout) vkDestroyPipelineLayout(p->device, p->pipeline_layout, NULL);
        if (p->descriptors) vkDestroyDescriptorPool(p->device, p->descriptors, NULL);
        if (p->layout) vkDestroyDescriptorSetLayout(p->device, p->layout, NULL);
        if (p->fence) vkDestroyFence(p->device, p->fence, NULL);
        if (p->pool) vkDestroyCommandPool(p->device, p->pool, NULL);
        for (unsigned i = 0; i < 5; i++) {
            Buffer *b = &p->buffers[i];
            if (b->mapped) vkUnmapMemory(p->device, b->memory);
            if (b->handle) vkDestroyBuffer(p->device, b->handle, NULL);
            if (b->memory) vkFreeMemory(p->device, b->memory, NULL);
        }
        vkDestroyDevice(p->device, NULL);
    }
    if (p->instance) vkDestroyInstance(p->instance, NULL);
    free(p->staged);
    free(p);
    g->backend = NULL; g->draw = NULL; g->sync = NULL; g->sync_range = NULL; g->flush_draw = NULL; g->stage_pages = NULL; g->fetch_pages = NULL;
}

static int device_supported(VkPhysicalDevice physical)
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceFloatControlsProperties floats = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &floats};
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    return features.shaderFloat64 && properties.apiVersion >= VK_API_VERSION_1_2 &&
        floats.shaderSignedZeroInfNanPreserveFloat32 &&
        properties.limits.maxUniformBufferRange >= sizeof(Constants) &&
        properties.limits.maxStorageBufferRange >= GS_VRAM_SIZE &&
        properties.limits.maxComputeWorkGroupInvocations >= 64 &&
        properties.limits.maxComputeWorkGroupSize[0] >= 8 && properties.limits.maxComputeWorkGroupSize[1] >= 8 &&
        properties.limits.maxComputeWorkGroupCount[0] >= 256 && properties.limits.maxComputeWorkGroupCount[1] >= 256;
}

int gs_vulkan_init(Gs *g)
{
    Gpu *p = calloc(1, sizeof *p);
    if (!p) return 0;
    g->backend = p;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Downhill Revived exact GS", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (!check(vkCreateInstance(&instance, NULL, &p->instance), "create instance")) {
        fprintf(stderr, "gs: install a Vulkan 1.2 driver matching the game's 32-bit architecture (for AMD: lib32-vulkan-radeon)\n");
        goto fail;
    }
    uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(p->instance, &count, NULL), "enumerate devices") || !count) goto fail;
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices) goto fail;
    VkResult result = vkEnumeratePhysicalDevices(p->instance, &count, devices);
    unsigned family = UINT32_MAX;
    if (result == VK_SUCCESS) for (unsigned d = 0; d < count && !p->physical; d++) {
        if (!device_supported(devices[d])) continue;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &n, NULL);
        VkQueueFamilyProperties *families = calloc(n, sizeof *families);
        if (!families) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &n, families);
        for (unsigned i = 0; i < n; i++) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            p->physical = devices[d]; family = i; break;
        }
        free(families);
    }
    free(devices);
    if (!p->physical) { fprintf(stderr, "gs: Vulkan exact requires Vulkan 1.2, compute, shaderFloat64, strict float32 and sufficient buffer/dispatch limits\n"); goto fail; }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(p->physical, &properties);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.shaderFloat64 = VK_TRUE};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .pEnabledFeatures = &features};
    if (!check(vkCreateDevice(p->physical, &device, NULL, &p->device), "create device")) goto fail;
    vkGetDeviceQueue(p->device, family, 0, &p->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    if (!check(vkCreateCommandPool(p->device, &pool, NULL, &p->pool), "create command pool")) goto fail;
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (!check(vkAllocateCommandBuffers(p->device, &commands, &p->command), "allocate commands")) goto fail;
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(vkCreateFence(p->device, &fence, NULL, &p->fence), "create fence")) goto fail;
    const VkDeviceSize sizes[] = {sizeof(Constants), GS_VRAM_SIZE, GS_VRAM_SIZE, MAX_DRAWS*sizeof(Primitive), MAX_BIN_WORDS*4};
    VkDescriptorSetLayoutBinding bindings[5] = {0};
    VkDescriptorBufferInfo buffers[5] = {0};
    for (unsigned i = 0; i < 5; i++) {
        if (!create_buffer(p, i, sizes[i])) goto fail;
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i,
            .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        buffers[i] = (VkDescriptorBufferInfo){.buffer = p->buffers[i].handle, .range = sizes[i]};
    }
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = bindings};
    if (!check(vkCreateDescriptorSetLayout(p->device, &layout, NULL, &p->layout), "create descriptors")) goto fail;
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    if (!check(vkCreateDescriptorPool(p->device, &descriptors, NULL, &p->descriptors), "create descriptor pool")) goto fail;
    VkDescriptorSetAllocateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->descriptors, .descriptorSetCount = 1, .pSetLayouts = &p->layout};
    if (!check(vkAllocateDescriptorSets(p->device, &set, &p->set), "allocate descriptors")) goto fail;
    VkWriteDescriptorSet writes[5] = {0};
    for (unsigned i = 0; i < 5; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->set, .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
        .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(p->device, 5, writes, 0, NULL);
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->layout};
    if (!check(vkCreatePipelineLayout(p->device, &pipeline_layout, NULL, &p->pipeline_layout), "create pipeline layout")) goto fail;
    VkShaderModule shader;
    VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof gs_spirv, .pCode = gs_spirv};
    if (!check(vkCreateShaderModule(p->device, &module, NULL, &shader), "create shader")) goto fail;
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}, .layout = p->pipeline_layout};
    result = vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &p->pipeline);
    vkDestroyShaderModule(p->device, shader, NULL);
    if (!check(result, "create compute pipeline")) goto fail;
    p->staged = malloc(GS_VRAM_SIZE);
    if (!p->staged) goto fail;
    p->bin_data = p->buffers[4].mapped;
    p->uploaded = p->state_version = UINT64_MAX;
    p->batch_limit = MAX_DRAWS;
    const char *batch = getenv("DHD_GS_BATCH");
    if (batch && atoi(batch) > 0 && atoi(batch) <= MAX_DRAWS) p->batch_limit = (unsigned)atoi(batch);
    memset(p->texture_dirty, 255, sizeof p->texture_dirty);
    g->stage_pages = stage_pages; g->fetch_pages = fetch_pages;
    g->sync = sync_vram; g->sync_range = sync_range; g->draw = draw; g->flush_draw = flush_draw;
    fprintf(stderr, "gs: Vulkan exact rasterizer: %s (shaderFloat64, ordered compute)\n", properties.deviceName);
    return 1;
fail:
    gs_gpu_destroy(g);
    return 0;
}
