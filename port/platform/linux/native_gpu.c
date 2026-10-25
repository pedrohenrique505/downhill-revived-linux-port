/* Vulkan graphics backend for native.c. The exact Z-probe backend has its own device/queue:
   native submission never drains exact work. Images and immutable descriptor/uniform slices
   live until the native fence completes. Positive-height viewports pair with DXC's invert-y.
   Offscreen scanout/readback are supported here; window presentation belongs to the host. */
#include "native_gpu.h"
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "gs_spirv.h"

struct NativeImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    unsigned w, h;
    int depth;
    uint32_t *pixels;                 /* immutable decode; uploaded by the render thread */
};
typedef struct NativeImage Image;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *map; VkDeviceSize size; } NativeBuffer;
typedef struct Garbage { struct Garbage *next; Image image; VkFramebuffer framebuffer; } Garbage;
typedef struct Pipeline { struct Pipeline *next; uint32_t blend; unsigned depth, mask, kind; VkPipeline pipeline; } Pipeline;
typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    int recording, pending;
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkDescriptorPool descriptor_pool;
    VkRenderPass passes[2];
    VkShaderModule shaders[8];
    VkPipeline scanout, convert;
    VkSampler samplers[8];
    Pipeline *pipelines;
    Image surfaces[NATIVE_SURFACES], converted[NATIVE_CONVERTS], display, dummy, dummy_depth;
    NativeBuffer arena, inverse, readback;
    VkDeviceSize used, alignment;
    Garbage *garbage;
} NativeVk;

static void vk_require(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) { fprintf(stderr, "gs native Vulkan: %s failed (%d)\n", what, r); exit(1); }
}
#define VK_CALL(call) vk_require((call), #call)

static unsigned memory_type(NativeVk *v, unsigned bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(v->physical, &p);
    for (unsigned i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "gs native Vulkan: no memory type for %x\n", flags); exit(1);
}
static NativeBuffer buffer_create(NativeVk *v, VkDeviceSize size, VkBufferUsageFlags usage)
{
    NativeBuffer b = {.size = size};
    VkBufferCreateInfo c = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VK_CALL(vkCreateBuffer(v->device, &c, NULL, &b.buffer));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(v->device, b.buffer, &r);
    VkMemoryAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
        .memoryTypeIndex = memory_type(v, r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CALL(vkAllocateMemory(v->device, &a, NULL, &b.memory));
    VK_CALL(vkBindBufferMemory(v->device, b.buffer, b.memory, 0));
    VK_CALL(vkMapMemory(v->device, b.memory, 0, size, 0, &b.map));
    return b;
}
static void buffer_free(NativeVk *v, NativeBuffer *b)
{
    if (b->map) vkUnmapMemory(v->device, b->memory);
    if (b->buffer) vkDestroyBuffer(v->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(v->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}
static void image_free(NativeVk *v, Image *i)
{
    if (i->view) vkDestroyImageView(v->device, i->view, NULL);
    if (i->image) vkDestroyImage(v->device, i->image, NULL);
    if (i->memory) vkFreeMemory(v->device, i->memory, NULL);
    free(i->pixels); memset(i, 0, sizeof *i);
}
static void retire(NativeVk *v)
{
    if (v->pending) {
        VK_CALL(vkWaitForFences(v->device, 1, &v->fence, VK_TRUE, UINT64_MAX));
        v->pending = 0;
    }
    while (v->garbage) {
        Garbage *g = v->garbage; v->garbage = g->next;
        image_free(v, &g->image);
        if (g->framebuffer) vkDestroyFramebuffer(v->device, g->framebuffer, NULL);
        free(g);
    }
}
static void begin(NativeVk *v)
{
    if (v->recording) return;
    retire(v);
    VK_CALL(vkResetCommandPool(v->device, v->pool, 0));
    VK_CALL(vkResetDescriptorPool(v->device, v->descriptor_pool, 0));
    v->used = 0;
    VkCommandBufferBeginInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CALL(vkBeginCommandBuffer(v->cmd, &c)); v->recording = 1;
}
static void submit(NativeVk *v)
{
    if (!v->recording) return;
    VK_CALL(vkEndCommandBuffer(v->cmd));
    VK_CALL(vkResetFences(v->device, 1, &v->fence));
    VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cmd};
    VK_CALL(vkQueueSubmit(v->queue, 1, &s, v->fence)); v->pending = 1; v->recording = 0;
}
static void barrier(NativeVk *v)
{
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
static VkDeviceSize upload(NativeVk *v, const void *p, size_t size)
{
    begin(v);
    VkDeviceSize at = (v->used + v->alignment - 1) / v->alignment * v->alignment;
    if (at + size > v->arena.size) { fprintf(stderr, "gs native Vulkan: field upload arena exhausted\n"); exit(1); }
    memcpy((char *)v->arena.map + at, p, size); v->used = at + size;
    return at;
}
static void discard_image(NativeVk *v, Image *i)
{
    if (!i->image) { free(i->pixels); memset(i, 0, sizeof *i); return; }
    Garbage *g = calloc(1, sizeof *g); if (!g) abort();
    g->image = *i; g->next = v->garbage; v->garbage = g; memset(i, 0, sizeof *i);
}
static void image_create(NativeVk *v, Image *i, unsigned w, unsigned h, int depth)
{
    begin(v);
    i->w = w; i->h = h; i->depth = depth;
    VkImageCreateInfo c = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)};
    VK_CALL(vkCreateImage(v->device, &c, NULL, &i->image));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(v->device, i->image, &r);
    VkMemoryAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
        .memoryTypeIndex = memory_type(v, r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK_CALL(vkAllocateMemory(v->device, &a, NULL, &i->memory)); VK_CALL(vkBindImageMemory(v->device, i->image, i->memory, 0));
    VkImageSubresourceRange range = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = c.format, .subresourceRange = range};
    VK_CALL(vkCreateImageView(v->device, &view, NULL, &i->view));
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = i->image, .subresourceRange = range,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (depth) { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(v->cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range); }
    else { VkClearColorValue zero = {0}; vkCmdClearColorImage(v->cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range); }
    barrier(v);
}
static void image_upload(NativeVk *v, Image *i, const uint32_t *pixels, unsigned x, unsigned y, unsigned w, unsigned h)
{
    VkDeviceSize at = upload(v, pixels, (size_t)w*h*4);
    barrier(v);
    VkBufferImageCopy copy = {.bufferOffset = at, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {(int)x, (int)y, 0}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(v->cmd, v->arena.buffer, i->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy); barrier(v);
}
static Image *texture_image(NativeVk *v, NativeTexture *t)
{
    Image *i = t ? (Image *)t->texture : NULL;
    if (i && !i->image) {
        image_create(v, i, i->w, i->h, 0);
        image_upload(v, i, i->pixels, 0, 0, i->w, i->h); free(i->pixels); i->pixels = NULL;
    }
    return i ? i : &v->dummy;
}
int native_gpu_texture(Native *n, NativeTexture *t, const uint32_t *pixels, unsigned w, unsigned h, int sharp, uint64_t key)
{
    (void)n; (void)sharp; (void)key;
    Image *i = calloc(1, sizeof *i); if (!i) return 0;
    i->pixels = malloc((size_t)w*h*4); if (!i->pixels) { free(i); return 0; }
    memcpy(i->pixels, pixels, (size_t)w*h*4); i->w = w; i->h = h;
    t->texture = (NativeImage *)i; t->view = (NativeView *)i; return 1;
}
void native_gpu_texture_free(Native *n, NativeTexture *t)
{
    EnterCriticalSection(&n->context_lock);
    Image *i = (Image *)t->texture;
    if (i) { discard_image(n->gpu, i); free(i); }
    t->texture = NULL; t->view = NULL; t->sharp = NULL;
    LeaveCriticalSection(&n->context_lock);
}
NativeView *native_gpu_pack_view(Native *n, TexPackEntry *e) { (void)n; (void)e; return NULL; }

static void image_copy(NativeVk *v, Image *from, Image *to, int sx, int sy, int x, int y, unsigned w, unsigned h)
{
    barrier(v);
    VkImageCopy c = {.srcSubresource = {from->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {sx, sy, 0}, .dstSubresource = {to->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {x, y, 0}, .extent = {w, h, 1}};
    vkCmdCopyImage(v->cmd, from->image, VK_IMAGE_LAYOUT_GENERAL, to->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c); barrier(v);
}
static Image snapshot(NativeVk *v, Image *from)
{
    Image i = {0}; image_create(v, &i, from->w, from->h, from->depth);
    image_copy(v, from, &i, 0, 0, 0, 0, from->w, from->h); return i;
}
int native_gpu_surface(Native *n, NativeSurface *s, const unsigned want[2])
{
    NativeVk *v = n->gpu; Image *i = &v->surfaces[s - n->surfaces];
    unsigned w = want[0]*s->scale, h = want[1]*s->scale;
    if (i->image && i->w >= w && i->h >= h) return 1;
    if (w < i->w) w = i->w; if (h < i->h) h = i->h;
    Image old = *i; memset(i, 0, sizeof *i); image_create(v, i, w, h, s->depth);
    if (old.image) { image_copy(v, &old, i, 0, 0, 0, 0, old.w, old.h); discard_image(v, &old); }
    s->size[0] = w; s->size[1] = h; return 1;
}
