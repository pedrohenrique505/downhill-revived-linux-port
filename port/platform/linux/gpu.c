/* Linux exact GS: shared HLSL rasterizer, ordered compute batches over 4 MB VRAM.
   Ordered dispatches share a submission, with separate immutable host inputs.
   Two fenced banks let CPU preparation overlap GPU execution; a bank is
   retired before any of its host input storage is reused. CPU transfers merge only
   dirty pages, preserving unrelated uploads and texture feedback snapshots.
   Native/upscaled rendering remains a separate backend. */
#include "gpu.h"
#include "native_gpu.h"
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
enum { DISPATCH_SLOTS = 16, SUBMISSIONS = 2, INPUT_SLOTS = DISPATCH_SLOTS * SUBMISSIONS };
/* A dispatch lasts as long as its longest tile list, walked in order by one
   workgroup while the others sit idle: fewer, larger batches shorten that
   chain. D3D11's MAX_DRAWS stays as it is. */
enum { BATCH_DRAWS = 4096 };
/* Register states one dispatch may hold (raster.hlsl, DHD_STATE_TABLE). */
enum { BATCH_STATES = 256 };
typedef struct {
    NativeHost host;  /* must be first: portable frontend exact-probe hooks */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pools[SUBMISSIONS];
    VkCommandBuffer commands[SUBMISSIONS];
    VkFence fences[SUBMISSIONS];
    unsigned bank, in_flight[SUBMISSIONS];
    VkDescriptorSetLayout layout;
    VkDescriptorPool descriptors;
    VkDescriptorSet sets[INPUT_SLOTS];
    VkDeviceSize strides[8];
    unsigned queued;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout copy_layout;
    VkPipelineLayout copy_pipeline_layout;
    VkPipeline copy_pipeline;
    VkDescriptorSet copy_sets[INPUT_SLOTS];
    /* b0, u0, t0, t1, t2, t3 with explicit DXC binding shifts; local_copy's b2;
       deferred-read staging */
    Buffer buffers[8];
    uint64_t submissions, bank_submission[SUBMISSIONS], staged_submission;
    Primitive pending[BATCH_DRAWS];
    Constants state, states[BATCH_STATES];
    unsigned state_count, state_new;
    uint32_t surface[2];
    unsigned pending_count, memberships, batch_limit, state_context;
    int dirty, texture_valid, batch_safe;
    uint64_t uploaded, state_version, texture_key, clamp_key, tex1_key, mip_key[2];
    uint32_t cpu_dirty[16], texture_dirty[16], texture_pages[16];
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
        .size = size, .usage = (index && index != 6 ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

/* Host input slices belong to one bank until its fence signals. Shared VRAM
   and texture snapshots stay ordered on the same Vulkan queue. */
static void retire(Gpu *p, unsigned bank)
{
    if (!p->in_flight[bank]) return;
    require(vkWaitForFences(p->device, 1, &p->fences[bank], VK_TRUE, UINT64_MAX), "wait compute");
    p->in_flight[bank] = 0;
}

static void begin_commands(Gpu *p)
{
    if (p->queued) return;
    retire(p, p->bank);
    require(vkResetCommandPool(p->device, p->pools[p->bank], 0), "reset commands");
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    require(vkBeginCommandBuffer(p->commands[p->bank], &begin), "begin commands");
}

/* Dispatches and snapshot copies remain FIFO-ordered across both banks on
   the same queue. Host access drains both fences; capacity submits without a
   wait so the next bank can be prepared while this one executes. */
static void submit(Gpu *p)
{
    if (!p->queued) return;
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(p->commands[p->bank], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    require(vkEndCommandBuffer(p->commands[p->bank]), "end commands");
    require(vkResetFences(p->device, 1, &p->fences[p->bank]), "reset fence");
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p->commands[p->bank]};
    require(vkQueueSubmit(p->queue, 1, &submit, p->fences[p->bank]), "submit compute");
    p->in_flight[p->bank] = 1;
    p->bank_submission[p->bank] = ++p->submissions;
    p->queued = 0;
    p->bank = (p->bank+1) % SUBMISSIONS;
}

/* After recording a command: submit at capacity, or at once when the other
   bank is done, so the GPU isn't left idle while more commands accumulate. */
static void queued(Gpu *p)
{
    unsigned other = (p->bank+1) % SUBMISSIONS;
    if (++p->queued == DISPATCH_SLOTS || !p->in_flight[other] ||
        vkGetFenceStatus(p->device, p->fences[other]) == VK_SUCCESS) submit(p);
}

static void drain(Gpu *p)
{
    submit(p);
    for (unsigned bank = 0; bank < SUBMISSIONS; bank++) retire(p, bank);
}

/* Same ordered 8x8 tile lists and independent-pixel proof as D3D11. Packed
   writes retain atomics; only proven full-word owners use ordinary stores. */
static void flush(Gpu *p)
{
    if (!p->pending_count) return;
    if (!p->queued) retire(p, p->bank);
    unsigned slot = p->bank*DISPATCH_SLOTS + p->queued;
    unsigned x0 = 2048, y0 = 2048, x1 = 0, y1 = 0;
    for (unsigned i = 0; i < p->pending_count; i++) {
        const uint32_t *b = p->pending[i].bounds;
        if (b[0]/8 < x0) x0 = b[0]/8;
        if (b[1]/8 < y0) y0 = b[1]/8;
        if ((b[0]+b[2]-1)/8 > x1) x1 = (b[0]+b[2]-1)/8;
        if ((b[1]+b[3]-1)/8 > y1) y1 = (b[1]+b[3]-1)/8;
    }
    unsigned width = x1-x0+1, height = y1-y0+1, tiles = width*height;
    uint32_t *bins = (uint32_t *)((uint8_t *)p->buffers[4].mapped + slot*p->strides[4]);
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
    p->state.info[0] = width; p->state.info[1] = p->batch_safe ? 1 : 0;
    memcpy((uint8_t *)p->buffers[0].mapped + slot*p->strides[0], &p->state, sizeof p->state);
    memcpy((uint8_t *)p->buffers[3].mapped + slot*p->strides[3], p->pending, p->pending_count*sizeof(Primitive));
    memcpy((uint8_t *)p->buffers[5].mapped + slot*p->strides[5], p->states, p->state_count*sizeof(Constants));
    begin_commands(p);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(p->commands[p->bank], VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdBindPipeline(p->commands[p->bank], VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(p->commands[p->bank], VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline_layout, 0, 1, &p->sets[slot], 0, NULL);
    vkCmdDispatch(p->commands[p->bank], width, height, 1);
    queued(p);
    p->pending_count = p->memberships = p->state_count = 0;
}
static void flush_draw(Gs *g) { flush(g->backend); }

static void sync_vram(Gs *g)
{
    Gpu *p = g->backend;
    if (!p->dirty) return;
    flush(p);
    drain(p);
    for (unsigned page = 0; page < 512; page++)
        if (p->cpu_dirty[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)g->vram+page*8192, (uint8_t *)p->buffers[1].mapped+page*8192, 8192);
    p->dirty = 0;
    memset(p->cpu_dirty, 0, sizeof p->cpu_dirty);
}
static void sync_range(Gs *g, unsigned address, unsigned bytes)
{
    Gpu *p = g->backend;
    if (!p->dirty || !bytes) return;
    if (address >= GS_VRAM_SIZE || bytes > GS_VRAM_SIZE-address) {
        sync_vram(g);
        return;
    }
    unsigned first = address/8192, last = (address+bytes-1)/8192;
    unsigned any = 0;
    for (unsigned page = first; page <= last; page++)
        any |= p->cpu_dirty[page/32] & (1u << (page&31));
    if (!any) return;
    flush(p);
    drain(p);
    for (unsigned page = first; page <= last; page++)
        if (p->cpu_dirty[page/32] & (1u << (page&31))) {
            memcpy((uint8_t *)g->vram+page*8192, (uint8_t *)p->buffers[1].mapped+page*8192, 8192);
            p->cpu_dirty[page/32] &= ~(1u << (page&31));
        }
}

/* Deferred reads (gs.c): the dirty pages are copied on the GPU in order with
   the draws, as D3D11 does, and only fetch_pages waits, for that submission. */
static void stage_pages(Gs *g, const uint32_t want[16], uint32_t staged[16])
{
    Gpu *p = g->backend;
    int any = 0;
    for (unsigned i = 0; i < 16; i++) any |= (staged[i] = p->dirty ? want[i] & p->cpu_dirty[i] : 0) != 0;
    if (!any) return;
    flush(p);
    begin_commands(p);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(p->commands[p->bank], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    VkBufferCopy copies[256];
    unsigned runs = 0;
    for (unsigned page = 0; page < 512; page++) {
        if (!(staged[page/32] & (1u << (page&31)))) continue;
        if (runs && copies[runs-1].srcOffset + copies[runs-1].size == page*8192u) copies[runs-1].size += 8192;
        else copies[runs++] = (VkBufferCopy){.srcOffset = page*8192u, .dstOffset = page*8192u, .size = 8192};
    }
    vkCmdCopyBuffer(p->commands[p->bank], p->buffers[1].handle, p->buffers[7].handle, runs, copies);
    p->queued++;
    submit(p);
    p->staged_submission = p->submissions;
}
static void fetch_pages(Gs *g, const uint32_t staged[16], uint32_t *image)
{
    Gpu *p = g->backend;
    for (unsigned bank = 0; bank < SUBMISSIONS; bank++)
        if (p->bank_submission[bank] == p->staged_submission) retire(p, bank);
    for (unsigned page = 0; page < 512; page++)
        if (staged[page/32] & (1u << (page&31)))
            memcpy((uint8_t *)image+page*8192, (uint8_t *)p->buffers[7].mapped+page*8192, 8192);
}
static void upload_pages(Gs *g)
{
    Gpu *p = g->backend;
    if (p->uploaded == g->pixels_written) return;
    flush(p);
    drain(p);
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
    if (gpu->host.clip_on) {
        minx = maximum(minx, gpu->host.clip[0]); miny = maximum(miny, gpu->host.clip[1]);
        maxx = minimum(maxx, gpu->host.clip[2]); maxy = minimum(maxy, gpu->host.clip[3]);
    }
    if (minx > maxx || miny > maxy) return;
    if (gpu->state_version != g->state_version || gpu->state_context != ctxt) {
        /* Primitives of another state join the batch when no pixel of either
           can reach another's memory: both pass the independence proof and
           draw into the same frame and Z layout, so each coordinate keeps its
           own words and the tile lists keep each pixel's order. */
        uint64_t zbuf = g->reg[GS_ZBUF_1 + ctxt], test = g->reg[GS_TEST_1 + ctxt];
        int z_used = !((zbuf >> 32 & 1) && (!(test >> 16 & 1) || (test >> 17 & 3) <= 1));
        uint32_t surface[2] = {(uint32_t)g->reg[GS_FRAME_1 + ctxt], (uint32_t)zbuf | (uint32_t)z_used << 31};
        int safe = gs_raster_independent_pixels(g, ctxt);
        if (!safe || gpu->state_count == BATCH_STATES || memcmp(surface, gpu->surface, sizeof surface)) flush(gpu);
        memcpy(gpu->surface, surface, sizeof surface);
        static const unsigned registers[] = {6,7,8,9,20,21,34,52,53,54,55,59,61,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79};
        for (unsigned r = 0; r < sizeof registers / sizeof *registers; r++) {
            unsigned i = registers[r];
            gpu->state.reg[i][0] = (uint32_t)g->reg[i]; gpu->state.reg[i][1] = (uint32_t)(g->reg[i] >> 32);
        }
        for (unsigned i = 0; i < 256; i++) gpu->state.palette[i] = gs_palette(g, g->reg[GS_TEX0_1 + ctxt], i);
        gpu->state_version = g->state_version; gpu->state_context = ctxt;
        gpu->batch_safe = safe;
        gpu->state_new = 1;
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
            begin_commands(gpu);
            VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(gpu->commands[gpu->bank], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
            /* Only pages written since the last snapshot differ from it: the rest
               of a full 4 MB copy is bandwidth an integrated GPU can't spare. */
            VkBufferCopy copies[256];
            unsigned runs = 0;
            for (unsigned page = 0; page < 512; page++) {
                if (!(gpu->texture_dirty[page/32] & (1u << (page&31)))) continue;
                if (runs && copies[runs-1].srcOffset + copies[runs-1].size == page*8192u) copies[runs-1].size += 8192;
                else copies[runs++] = (VkBufferCopy){.srcOffset = page*8192u, .dstOffset = page*8192u, .size = 8192};
            }
            vkCmdCopyBuffer(gpu->commands[gpu->bank], gpu->buffers[1].handle, gpu->buffers[2].handle, runs, copies);
            queued(gpu);
            memset(gpu->texture_dirty, 0, sizeof gpu->texture_dirty);
            break;
        }
    }
    if (gpu->state_new || !gpu->state_count) {
        gpu->state.info[1] = gpu->batch_safe ? 1 : 0;
        gpu->states[gpu->state_count++] = gpu->state;
        gpu->state_new = 0;
    }
    Primitive *p = &gpu->pending[gpu->pending_count++];
    memcpy(p->position, c.position, sizeof p->position); memcpy(p->uv, c.uv, sizeof p->uv);
    memcpy(p->stq, c.stq, sizeof p->stq); memcpy(p->info, c.info, sizeof p->info);
    memcpy(p->bounds, c.bounds, sizeof p->bounds);
    memcpy(p->depth, c.depth, sizeof p->depth);
    p->info[3] = gpu->state_count - 1;
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


/* TRXDIR 2 on the GPU, ordered with the draws, as D3D11 does: the CPU path
   would wait for all queued drawing, copy and upload the pages back. */
static int local_copy(Gs *g)
{
    Gpu *gpu = g->backend;
    uint64_t b = g->reg[GS_BITBLTBUF], p = g->reg[GS_TRXPOS], r = g->reg[GS_TRXREG];
    uint32_t c[12] = {0}, from[16] = {0}, to[16] = {0};
    unsigned width = (unsigned)r & 4095, height = (unsigned)(r >> 32) & 4095;
    c[0] = (unsigned)(b >> 24) & 63; c[1] = (unsigned)b & 0x3FFF; c[2] = (unsigned)(b >> 16) & 63;
    c[3] = (unsigned)p & 2047; c[4] = (unsigned)(p >> 16) & 2047;
    c[5] = (unsigned)(b >> 56) & 63; c[6] = (unsigned)(b >> 32) & 0x3FFF; c[7] = (unsigned)(b >> 48) & 63;
    c[8] = width; c[9] = height; c[10] = (unsigned)(p >> 32) & 2047; c[11] = (unsigned)(p >> 48) & 2047;
    if (!width || !height || c[3] + width > 2048 || c[4] + height > 2048 ||
        c[10] + width > 2048 || c[11] + height > 2048)
        return 0;
    gs_mark_pages(from, c[0], c[1], c[2], c[3], c[4], c[3] + width - 1, c[4] + height - 1);
    gs_mark_pages(to, c[5], c[6], c[7], c[10], c[11], c[10] + width - 1, c[11] + height - 1);
    for (unsigned i = 0; i < 16; i++) if (from[i] & to[i]) return 0;
    upload_pages(g);
    flush(gpu);
    if (!gpu->queued) retire(gpu, gpu->bank);
    unsigned slot = gpu->bank*DISPATCH_SLOTS + gpu->queued;
    memcpy((uint8_t *)gpu->buffers[6].mapped + slot*gpu->strides[6], c, sizeof c);
    begin_commands(gpu);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(gpu->commands[gpu->bank], VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdBindPipeline(gpu->commands[gpu->bank], VK_PIPELINE_BIND_POINT_COMPUTE, gpu->copy_pipeline);
    vkCmdBindDescriptorSets(gpu->commands[gpu->bank], VK_PIPELINE_BIND_POINT_COMPUTE, gpu->copy_pipeline_layout, 0, 1, &gpu->copy_sets[slot], 0, NULL);
    vkCmdDispatch(gpu->commands[gpu->bank], (width + 7) / 8, (height + 7) / 8, 1);
    queued(gpu);
    for (unsigned i = 0; i < 16; i++) { gpu->cpu_dirty[i] |= to[i]; gpu->texture_dirty[i] |= to[i]; }
    gpu->dirty = 1;
    return 1;
}

void gs_gpu_destroy(Gs *g)
{
    Gpu *p = g->backend;
    if (!p) return;
#ifdef DHD_NATIVE_VULKAN
    if (p->host.native) native_vulkan_destroy(p->host.native);
#endif
    if (p->device) {
        vkDeviceWaitIdle(p->device);
        if (p->pipeline) vkDestroyPipeline(p->device, p->pipeline, NULL);
        if (p->copy_pipeline) vkDestroyPipeline(p->device, p->copy_pipeline, NULL);
        if (p->copy_pipeline_layout) vkDestroyPipelineLayout(p->device, p->copy_pipeline_layout, NULL);
        if (p->copy_layout) vkDestroyDescriptorSetLayout(p->device, p->copy_layout, NULL);
        if (p->pipeline_layout) vkDestroyPipelineLayout(p->device, p->pipeline_layout, NULL);
        if (p->descriptors) vkDestroyDescriptorPool(p->device, p->descriptors, NULL);
        if (p->layout) vkDestroyDescriptorSetLayout(p->device, p->layout, NULL);
        for (unsigned bank = 0; bank < SUBMISSIONS; bank++) {
            if (p->fences[bank]) vkDestroyFence(p->device, p->fences[bank], NULL);
            if (p->pools[bank]) vkDestroyCommandPool(p->device, p->pools[bank], NULL);
        }
        for (unsigned i = 0; i < 8; i++) {
            Buffer *b = &p->buffers[i];
            if (b->mapped) vkUnmapMemory(p->device, b->memory);
            if (b->handle) vkDestroyBuffer(p->device, b->handle, NULL);
            if (b->memory) vkFreeMemory(p->device, b->memory, NULL);
        }
        vkDestroyDevice(p->device, NULL);
    }
    if (p->instance) vkDestroyInstance(p->instance, NULL);
    free(p);
    g->backend = NULL; g->draw = NULL; g->sync = NULL; g->sync_range = NULL; g->flush_draw = NULL; g->local_copy = NULL; g->stage_pages = NULL; g->fetch_pages = NULL;
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
    const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("DHD_VK_VALIDATE")) { instance.enabledLayerCount = 1; instance.ppEnabledLayerNames = &validation_layer; }
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
    for (unsigned bank = 0; bank < SUBMISSIONS; bank++) {
        VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
        if (!check(vkCreateCommandPool(p->device, &pool, NULL, &p->pools[bank]), "create command pool")) goto fail;
        VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = p->pools[bank], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (!check(vkAllocateCommandBuffers(p->device, &commands, &p->commands[bank]), "allocate commands")) goto fail;
        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!check(vkCreateFence(p->device, &fence, NULL, &p->fences[bank]), "create fence")) goto fail;
    }
    const VkDeviceSize sizes[] = {sizeof(Constants), GS_VRAM_SIZE, GS_VRAM_SIZE, BATCH_DRAWS*sizeof(Primitive), MAX_BIN_WORDS*4,
                                  BATCH_STATES*sizeof(Constants)};
    VkDescriptorSetLayoutBinding bindings[6] = {0};
    VkDescriptorBufferInfo buffers[6] = {0};
    for (unsigned i = 0; i < 6; i++) {
        VkDeviceSize alignment = i ? properties.limits.minStorageBufferOffsetAlignment : properties.limits.minUniformBufferOffsetAlignment;
        p->strides[i] = (sizes[i]+alignment-1)/alignment*alignment;
        unsigned slots = (i == 0 || i >= 3) ? INPUT_SLOTS : 1;
        if (!create_buffer(p, i, p->strides[i]*slots)) goto fail;
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i,
            .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        buffers[i] = (VkDescriptorBufferInfo){.buffer = p->buffers[i].handle, .range = sizes[i]};
    }
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 6, .pBindings = bindings};
    if (!check(vkCreateDescriptorSetLayout(p->device, &layout, NULL, &p->layout), "create descriptors")) goto fail;
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2*INPUT_SLOTS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6*INPUT_SLOTS}};
    VkDescriptorPoolCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2*INPUT_SLOTS, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    if (!check(vkCreateDescriptorPool(p->device, &descriptors, NULL, &p->descriptors), "create descriptor pool")) goto fail;
    for (unsigned slot = 0; slot < INPUT_SLOTS; slot++) {
        VkDescriptorSetAllocateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = p->descriptors, .descriptorSetCount = 1, .pSetLayouts = &p->layout};
        if (!check(vkAllocateDescriptorSets(p->device, &set, &p->sets[slot]), "allocate descriptors")) goto fail;
        VkWriteDescriptorSet writes[6] = {0};
        for (unsigned i = 0; i < 6; i++) {
            buffers[i].offset = (i == 0 || i >= 3) ? slot*p->strides[i] : 0;
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = p->sets[slot], .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
                .pBufferInfo = &buffers[i]};
        }
        vkUpdateDescriptorSets(p->device, 6, writes, 0, NULL);
    }
    /* local_copy: GS VRAM (u0, binding 1) and its constants (b2 shifted to 10) */
    p->strides[6] = (48+properties.limits.minUniformBufferOffsetAlignment-1)/properties.limits.minUniformBufferOffsetAlignment*properties.limits.minUniformBufferOffsetAlignment;
    if (!create_buffer(p, 6, p->strides[6]*INPUT_SLOTS)) goto fail;
    VkDescriptorSetLayoutBinding copy_bindings[2] = {bindings[1], {.binding = 10, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo copy_layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = copy_bindings};
    if (!check(vkCreateDescriptorSetLayout(p->device, &copy_layout, NULL, &p->copy_layout), "create copy descriptors")) goto fail;
    for (unsigned slot = 0; slot < INPUT_SLOTS; slot++) {
        VkDescriptorSetAllocateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = p->descriptors, .descriptorSetCount = 1, .pSetLayouts = &p->copy_layout};
        if (!check(vkAllocateDescriptorSets(p->device, &set, &p->copy_sets[slot]), "allocate copy descriptors")) goto fail;
        VkDescriptorBufferInfo vram = {.buffer = p->buffers[1].handle, .range = GS_VRAM_SIZE};
        VkDescriptorBufferInfo constants = {.buffer = p->buffers[6].handle, .offset = slot*p->strides[6], .range = 48};
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->copy_sets[slot], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &vram},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->copy_sets[slot], .dstBinding = 10, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &constants}};
        vkUpdateDescriptorSets(p->device, 2, writes, 0, NULL);
    }
    VkPipelineLayoutCreateInfo copy_pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->copy_layout};
    if (!check(vkCreatePipelineLayout(p->device, &copy_pipeline_layout, NULL, &p->copy_pipeline_layout), "create copy pipeline layout")) goto fail;
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
    module.codeSize = sizeof gs_spirv_copy; module.pCode = gs_spirv_copy;
    if (!check(vkCreateShaderModule(p->device, &module, NULL, &shader), "create copy shader")) goto fail;
    pipeline.stage.module = shader; pipeline.stage.pName = "local_copy"; pipeline.layout = p->copy_pipeline_layout;
    result = vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &p->copy_pipeline);
    vkDestroyShaderModule(p->device, shader, NULL);
    if (!check(result, "create copy pipeline")) goto fail;
    if (!create_buffer(p, 7, GS_VRAM_SIZE)) goto fail;
    p->uploaded = p->state_version = UINT64_MAX;
    p->batch_limit = BATCH_DRAWS;
    const char *batch = getenv("DHD_GS_BATCH");
    if (batch && atoi(batch) > 0 && atoi(batch) <= BATCH_DRAWS) p->batch_limit = (unsigned)atoi(batch);
    memset(p->texture_dirty, 255, sizeof p->texture_dirty);
    g->stage_pages = stage_pages; g->fetch_pages = fetch_pages;
    g->sync = sync_vram; g->sync_range = sync_range; g->draw = draw; g->flush_draw = flush_draw;
    g->local_copy = local_copy;
    fprintf(stderr, "gs: Vulkan exact rasterizer: %s (shaderFloat64, ordered compute)\n", properties.deviceName);
    return 1;
fail:
    gs_gpu_destroy(g);
    return 0;
}
