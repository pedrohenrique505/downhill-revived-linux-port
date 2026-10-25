/* Vulkan graphics backend for native.c. The exact Z-probe backend has its own device/queue:
   native submission never drains exact work. Images and immutable descriptor/uniform slices
   live until the native fence completes. Positive-height viewports pair with DXC's invert-y.
   Offscreen scanout/readback are supported here; window presentation belongs to the host. */
#include "native_gpu.h"
#include "../../gs/present.h"
void pacing_present(double due, double began);
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
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
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage *backbuffers;
    VkSemaphore *finished, acquired;
    unsigned image_count, family, width, height, requested_width, requested_height;
    unsigned window_width, window_height; /* published by the SDL/window thread */
    VkPresentModeKHR present_mode;
    int recreate, point;
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

/* One immutable descriptor set per operation. All optional resources are valid zero images;
   the shared shaders sometimes issue loads before selecting their result. */
static const unsigned descriptor_bindings[] = {1,3,4,5,20,21,22,23,24,25,26,27,28,29,30,31,32,48,49,65,66};
static VkDescriptorType descriptor_type(unsigned b)
{
    if (b < 16) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (b == 22) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (b >= 65) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (b >= 48) return VK_DESCRIPTOR_TYPE_SAMPLER;
    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}
static VkDescriptorSet descriptors(NativeVk *v, unsigned constant_binding, const void *constants, size_t size,
                                    Image *images[67], unsigned sampler)
{
    VkDeviceSize at = upload(v, constants, size);
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &v->descriptors};
    VK_CALL(vkAllocateDescriptorSets(v->device, &a, &set));
    VkDescriptorImageInfo ii[21]; VkDescriptorBufferInfo bi[21]; VkWriteDescriptorSet writes[21];
    for (unsigned j = 0; j < 21; j++) {
        unsigned b = descriptor_bindings[j]; VkDescriptorType type = descriptor_type(b);
        Image *image = images && images[b] ? images[b] : (b >= 29 && b <= 32 ? &v->dummy_depth : &v->dummy);
        ii[j] = (VkDescriptorImageInfo){.imageView = image->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .sampler = v->samplers[b == 49 ? sampler : 0]};
        bi[j] = b == 22 ? (VkDescriptorBufferInfo){v->inverse.buffer, 0, v->inverse.size} :
            (VkDescriptorBufferInfo){v->arena.buffer, at, b == constant_binding ? size : 16};
        writes[j] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = b, .descriptorCount = 1, .descriptorType = type,
            .pImageInfo = &ii[j], .pBufferInfo = &bi[j]};
    }
    vkUpdateDescriptorSets(v->device, 21, writes, 0, NULL); return set;
}
static VkBlendFactor blend_factor(unsigned factor)
{
    switch (factor) {
    case BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case BLEND_SA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case BLEND_ISA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case BLEND_DA: return VK_BLEND_FACTOR_DST_ALPHA;
    case BLEND_IDA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BLEND_F: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BLEND_IF: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: fprintf(stderr, "gs native Vulkan: unsupported blend factor %u\n", factor); exit(1);
    }
}
static VkPipeline graphics_pipeline(NativeVk *v, unsigned kind, uint32_t blend, unsigned depth, unsigned mask)
{
    for (Pipeline *p = v->pipelines; p; p = p->next)
        if (p->kind == kind && p->blend == blend && p->depth == depth && p->mask == mask) return p->pipeline;
    Pipeline *p = calloc(1, sizeof *p); if (!p) abort();
    p->kind = kind; p->blend = blend; p->depth = depth; p->mask = mask;
    static const char *entries[] = {"native_vertex", "native_pixel", "native_scanout", "native_convert",
        "present_vertex", "present_pixel", "native_blit", "native_upload_alpha"};
    unsigned vs = kind ? 4 : 0, ps = kind == 1 ? 6 : kind == 2 ? 7 : 1;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = v->shaders[vs], .pName = entries[vs]},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = v->shaders[ps], .pName = entries[ps]}};
    VkVertexInputBindingDescription binding = {0, sizeof(NativeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(NativeVertex,x)},
        {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(NativeVertex,s)}, {2,0,VK_FORMAT_R8G8B8A8_UINT,offsetof(NativeVertex,rgba)}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = kind ? 0 : 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = kind ? 0 : 3, .pVertexAttributeDescriptions = attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_TRUE, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    static const VkCompareOp compare[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
    VkPipelineDepthStencilStateCreateInfo z = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = !!(depth & 8), .depthWriteEnable = !!(depth & 4), .depthCompareOp = compare[depth & 3]};
    unsigned op = blend >> 11 & 7;
    VkPipelineColorBlendAttachmentState attachment = {.blendEnable = blend & 1,
        .srcColorBlendFactor = blend ? blend_factor(blend >> 1 & 31) : VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = blend ? blend_factor(blend >> 6 & 31) : VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = op == 2 ? VK_BLEND_OP_SUBTRACT : op == 3 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = mask};
    VkPipelineColorBlendStateCreateInfo color = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo c = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &ms,
        .pDepthStencilState = &z, .pColorBlendState = &color, .pDynamicState = &dynamic,
        .layout = v->layout, .renderPass = v->passes[!!(depth & 8)]};
    VK_CALL(vkCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &c, NULL, &p->pipeline));
    p->next = v->pipelines; v->pipelines = p; return p->pipeline;
}
static void draw(NativeVk *v, Image *target, Image *depth, VkPipeline pipeline, VkDescriptorSet set,
                 VkViewport viewport, VkRect2D rect, unsigned first, unsigned count, unsigned fix)
{
    if (!rect.extent.width || !rect.extent.height) return;
    barrier(v);
    VkImageView views[] = {target->view, depth ? depth->view : VK_NULL_HANDLE};
    VkFramebuffer framebuffer;
    VkFramebufferCreateInfo c = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = v->passes[depth != NULL], .attachmentCount = depth ? 2 : 1, .pAttachments = views,
        .width = target->w, .height = target->h, .layers = 1};
    VK_CALL(vkCreateFramebuffer(v->device, &c, NULL, &framebuffer));
    Garbage *g = calloc(1, sizeof *g); if (!g) abort();
    g->framebuffer = framebuffer; g->next = v->garbage; v->garbage = g;
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c.renderPass,
        .framebuffer = framebuffer, .renderArea = {{0,0},{target->w,target->h}}};
    vkCmdBeginRenderPass(v->cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 0, 1, &set, 0, NULL);
    vkCmdSetViewport(v->cmd, 0, 1, &viewport); vkCmdSetScissor(v->cmd, 0, 1, &rect);
    float f = fix/128.0f, factors[4] = {f,f,f,f}; vkCmdSetBlendConstants(v->cmd, factors);
    vkCmdDraw(v->cmd, count, 1, first, 0); vkCmdEndRenderPass(v->cmd); barrier(v);
}
static void native_blit(NativeVk *v, Image *from, unsigned S, int sx, int sy, Image *to, unsigned T,
                        int x, int y, int w, int h, Image *previous)
{
    if (x+w > (int)(to->w/T)) w = (int)(to->w/T)-x;
    if (y+h > (int)(to->h/T)) h = (int)(to->h/T)-y;
    if (w <= 0 || h <= 0) return;
    float map[] = {(float)(previous ? x : sx-x), (float)(previous ? y : sy-y), (float)T, (float)S};
    Image *images[67] = {[23] = from, [21] = previous};
    VkDescriptorSet set = descriptors(v, 5, map, sizeof map, images, 0);
    VkViewport viewport = {(float)(x*(int)T), (float)(y*(int)T), (float)(w*(int)T), (float)(h*(int)T), 0, 1};
    VkRect2D rect = {{x*(int)T,y*(int)T},{w*T,h*T}};
    draw(v, to, NULL, graphics_pipeline(v, previous ? 2 : 1, 0, 0, 15), set, viewport, rect, 0, 3, 0);
}
static Image *native_run_convert(Native *n, NativeFrame *f, unsigned index)
{
    NativeVk *v = n->gpu; NativeConvert *c = &f->converts[index];
    NativeConvertConstants constants = c->constants; constants.tex[3] = 0;
    Image *out = &v->converted[index];
    if (out->w != constants.size[0] || out->h != constants.size[1]) {
        discard_image(v, out); image_create(v, out, constants.size[0], constants.size[1], 0);
    }
    Image *images[67] = {[24] = texture_image(v, c->base), [66] = out};
    for (unsigned j = 0; j < 4; j++) {
        NativeSurface *s = c->surface[j] >= 0 ? &n->surfaces[c->surface[j]] : NULL;
        if (s) images[25+j+(s->depth ? 4 : 0)] = &v->surfaces[c->surface[j]];
        constants.tex[3] |= (s ? s->scale : 1) << (j*8);
    }
    VkDescriptorSet set = descriptors(v, 4, &constants, sizeof constants, images, 0);
    barrier(v); vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->convert);
    vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(v->cmd, (out->w+7)/8, (out->h+7)/8, 1); barrier(v);
    n->stats.conversions_run++; return out;
}
static int min_int(int a, int b) { return a < b ? a : b; }
void native_gpu_draw(Native *n, NativeFrame *f, const NativeVertex *vertices)
{
    NativeVk *v = n->gpu; begin(v);
    VkDeviceSize vertex_offset = 0;
    if (f->vertex_count) vertex_offset = upload(v, vertices, (size_t)f->vertex_count*sizeof *vertices);
    vkCmdBindVertexBuffers(v->cmd, 0, 1, &v->arena.buffer, &vertex_offset);
    int last_convert = -1; Image *converted = NULL;
    for (unsigned j = 0; j < f->command_count; j++) {
        const NativeCommand *k = &f->commands[j];
        NativeSurface *s = &n->surfaces[k->target]; Image *target = &v->surfaces[k->target];
        unsigned T = s->scale;
        if (!target->image) continue;
        if (k->kind != NATIVE_DRAW) last_convert = -1;
        if (k->kind == NATIVE_IMPORT || k->kind == NATIVE_ALPHA_UPLOAD) {
            if (k->kind == NATIVE_IMPORT && T == 1) image_upload(v, target, k->pixels, k->x, k->y, k->w, k->h);
            else {
                unsigned w = k->kind == NATIVE_ALPHA_UPLOAD ? 64 : k->w, h = k->kind == NATIVE_ALPHA_UPLOAD ? 32 : k->h;
                Image page = {0}, previous = {0}; image_create(v, &page, w, h, 0);
                image_upload(v, &page, k->pixels, 0, 0, w, h);
                if (k->kind == NATIVE_ALPHA_UPLOAD) previous = snapshot(v, target);
                native_blit(v, &page, 1, 0, 0, target, T, k->x, k->y, w, h, previous.image ? &previous : NULL);
                discard_image(v, &previous); discard_image(v, &page);
            }
            continue;
        }
        if (k->kind == NATIVE_COPY) {
            Image *from = &v->surfaces[k->source]; unsigned S = n->surfaces[k->source].scale;
            int w = min_int(k->w, min_int((int)(from->w/S)-k->sx, (int)(target->w/T)-k->x));
            int h = min_int(k->h, min_int((int)(from->h/S)-k->sy, (int)(target->h/T)-k->y));
            if (!from->image || w <= 0 || h <= 0) continue;
            Image copy = {0}; if (from == target) { copy = snapshot(v, from); from = &copy; }
            if (S == T) image_copy(v, from, target, k->sx*S, k->sy*S, k->x*T, k->y*T, w*T, h*T);
            else native_blit(v, from, S, k->sx, k->sy, target, T, k->x, k->y, w, h, NULL);
            discard_image(v, &copy); continue;
        }
        if (!k->count) continue;
        Image *depth = k->depth >= 0 && (k->depth_key & 8) ? &v->surfaces[k->depth] : NULL;
        if (depth && (!depth->image || depth->w != target->w || depth->h != target->h)) depth = NULL;
        Image *texture = &v->dummy, copy = {0}; unsigned S = 1;
        if (k->texture_surface >= 0) {
            texture = &v->surfaces[k->texture_surface]; S = n->surfaces[k->texture_surface].scale;
            if (texture == target) { copy = snapshot(v, target); texture = &copy; }
        } else if (k->texture) texture = texture_image(v, k->texture);
        else if (k->convert >= 0) {
            if (k->convert != last_convert) converted = native_run_convert(n, f, k->convert);
            texture = converted; last_convert = k->convert;
        }
        if (k->convert < 0) last_convert = -1;
        NativeConstants constants = k->constants; constants.scale = T | S << 8 | (S<T ? S:T) << 16;
        Image *images[67] = {[20] = texture};
        VkDescriptorSet set = descriptors(v, 3, &constants, sizeof constants, images, 0);
        VkViewport viewport = {0,0,2048.0f*T,2048.0f*T,0,1};
        int left = k->scissor.left*T, top = k->scissor.top*T;
        int right = min_int(k->scissor.right*T, target->w), bottom = min_int(k->scissor.bottom*T, target->h);
        if (left < 0) left = 0; if (top < 0) top = 0;
        if (right > left && bottom > top) {
            VkRect2D rect = {{left,top},{right-left,bottom-top}};
            draw(v, target, depth, graphics_pipeline(v, 0, k->blend_key, depth ? k->depth_key : 0, k->color_mask),
                set, viewport, rect, k->first, k->count, k->blend_fix);
        }
        discard_image(v, &copy);
    }
}
static Image *display_size(Native *n, unsigned w, unsigned h)
{
    NativeVk *v = n->gpu;
    if (v->display.w != w || v->display.h != h) {
        discard_image(v, &v->display); image_create(v, &v->display, w, h, 0);
    }
    n->display_width = w; n->display_height = h; return &v->display;
}
int native_gpu_scanout(Native *n, NativeFrame *f)
{
    NativeVk *v = n->gpu;
    if (f->display_surface < 0) { submit(v); return 0; }
    unsigned scale = n->surfaces[f->display_surface].scale;
    uint32_t d[12]; memcpy(d, f->display, sizeof d); d[5] *= scale; d[6] *= scale; d[8] = scale;
    Image *out = display_size(n, d[5], d[6]);
    Image *images[67] = {[21] = &v->surfaces[f->display_surface], [65] = out};
    VkDescriptorSet set = descriptors(v, 1, d, sizeof d, images, 0);
    barrier(v); vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->scanout);
    vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(v->cmd, (d[5]+7)/8, (d[6]+7)/8, 1); barrier(v); submit(v); return 1;
}
int native_gpu_movie(Native *n, NativeFrame *f)
{
    NativeVk *v = n->gpu; Image *out = display_size(n, f->movie_width, f->movie_height);
    image_upload(v, out, f->movie, 0, 0, f->movie_width, f->movie_height); submit(v); return 1;
}
int native_gpu_read(Native *n, uint32_t *pixels)
{
    NativeVk *v = n->gpu; if (!v->display.image) return 0;
    submit(v); retire(v);
    size_t size = (size_t)n->display_width*n->display_height*4;
    if (v->readback.size < size) { buffer_free(v, &v->readback); v->readback = buffer_create(v, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT); }
    begin(v); barrier(v);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent = {n->display_width,n->display_height,1}};
    vkCmdCopyImageToBuffer(v->cmd, v->display.image, VK_IMAGE_LAYOUT_GENERAL, v->readback.buffer, 1, &c);
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    submit(v); retire(v); memcpy(pixels, v->readback.map, size); return 1;
}
/* SDL window APIs stay on the EE thread; Vulkan presentation stays on native_thread. */
void native_vulkan_window_size(Native *n, unsigned width, unsigned height)
{
    NativeVk *v = n->gpu;
    __atomic_store_n(&v->window_width, width, __ATOMIC_RELEASE);
    __atomic_store_n(&v->window_height, height, __ATOMIC_RELEASE);
}
unsigned native_gpu_window_height(Native *n)
{
    NativeVk *v = n->gpu;
    return __atomic_load_n(&v->window_height, __ATOMIC_ACQUIRE);
}
static void swapchain_free(NativeVk *v)
{
    if (!v->swapchain) return;
    /* Presentation has its own lifetime beyond the submission fence. */
    VK_CALL(vkDeviceWaitIdle(v->device));
    for (unsigned i = 0; i < v->image_count; i++) vkDestroySemaphore(v->device, v->finished[i], NULL);
    free(v->finished); free(v->backbuffers);
    v->finished = NULL; v->backbuffers = NULL; v->image_count = 0;
    vkDestroySwapchainKHR(v->device, v->swapchain, NULL); v->swapchain = VK_NULL_HANDLE;
}
static int swapchain_create(Native *n)
{
    NativeVk *v = n->gpu;
    unsigned w = __atomic_load_n(&v->window_width, __ATOMIC_ACQUIRE);
    unsigned h = __atomic_load_n(&v->window_height, __ATOMIC_ACQUIRE);
    if (!w || !h) return 0;
    if (v->swapchain && !v->recreate && w == v->requested_width && h == v->requested_height) return 1;
    submit(v); retire(v); swapchain_free(v);
    VkSurfaceCapabilitiesKHR caps;
    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->physical, v->surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = w < caps.minImageExtent.width ? caps.minImageExtent.width : w > caps.maxImageExtent.width ? caps.maxImageExtent.width : w;
        extent.height = h < caps.minImageExtent.height ? caps.minImageExtent.height : h > caps.maxImageExtent.height ? caps.maxImageExtent.height : h;
    }
    if (!extent.width || !extent.height) return 0;
    uint32_t count = 0;
    VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(v->physical, v->surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count, sizeof *formats); if (!formats) abort();
    VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(v->physical, v->surface, &count, formats));
    VkSurfaceFormatKHR format = {0};
    for (unsigned i = 0; i < count; i++)
        if ((formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format = formats[i]; break; }
    free(formats);
    VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(v->physical, format.format, &properties);
    if (!format.format || !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "gs native Vulkan: surface requires UNORM transfer/blit destination support\n"); exit(1);
    }
    v->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    /* The field clock or DHD_INTERP_HZ paces these modes; otherwise the display does. */
    const char *vsync = getenv("DHD_VSYNC");
    int fifo = n->interp ? !n->interp_hz : vsync && atoi(vsync) != 0;
    if (!fifo) {
        VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(v->physical, v->surface, &count, NULL));
        VkPresentModeKHR *modes = calloc(count, sizeof *modes); if (!modes) abort();
        VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(v->physical, v->surface, &count, modes));
        for (unsigned i = 0; i < count; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) v->present_mode = modes[i];
        for (unsigned i = 0; i < count; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) v->present_mode = modes[i];
        free(modes);
    }
    unsigned images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha;
    VkSwapchainCreateInfoKHR c = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = v->surface, .minImageCount = images, .imageFormat = format.format, .imageColorSpace = format.colorSpace,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = v->present_mode, .clipped = VK_TRUE};
    VK_CALL(vkCreateSwapchainKHR(v->device, &c, NULL, &v->swapchain));
    VK_CALL(vkGetSwapchainImagesKHR(v->device, v->swapchain, &v->image_count, NULL));
    v->backbuffers = calloc(v->image_count, sizeof *v->backbuffers);
    v->finished = calloc(v->image_count, sizeof *v->finished);
    if (!v->backbuffers || !v->finished) abort();
    VK_CALL(vkGetSwapchainImagesKHR(v->device, v->swapchain, &v->image_count, v->backbuffers));
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (unsigned i = 0; i < v->image_count; i++) VK_CALL(vkCreateSemaphore(v->device, &sem, NULL, &v->finished[i]));
    v->requested_width = w; v->requested_height = h; v->recreate = 0;
    /* Actual surface extent can differ from the last SDL event during resize. */
    v->width = extent.width; v->height = extent.height;
    fprintf(stderr, "gs native Vulkan: swapchain %ux%u mode=%u images=%u\n", v->width, v->height, v->present_mode, v->image_count);
    return 1;
}
int native_gpu_present(Native *n)
{
    NativeVk *v = n->gpu;
    if (!v->surface || !v->display.image) return 0;
    if (!swapchain_create(n)) { Sleep(10); return 0; }
    begin(v); /* retires the previous submission before reusing the acquire semaphore */
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(v->device, v->swapchain, 100000000, v->acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { v->recreate = 1; return 0; }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return 0;
    if (r != VK_SUBOPTIMAL_KHR) vk_require(r, "vkAcquireNextImageKHR");
    else v->recreate = 1;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = v->backbuffers[index], .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue black = {.float32 = {0,0,0,1}};
    vkCmdClearColorImage(v->cmd, b.image, b.newLayout, &black, 1, &range); barrier(v);
    float fw, fh; native_viewport(n, v->width, v->height, &fw, &fh);
    int x = (int)floorf((v->width-fw)/2), y = (int)floorf((v->height-fh)/2);
    int sx = 0, sy = 0, sw = v->display.w, sh = v->display.h;
    int view = gs_native_view - 1;
    if (view >= 0 && view < 4) { sw /= 2; sh /= 2; sx = (view & 1)*sw; sy = (view >> 1)*sh; }
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets = {{sx,sy,0},{sx+sw,sy+sh,1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets = {{x,y,0},{x+(int)fw,y+(int)fh,1}}};
    vkCmdBlitImage(v->cmd, v->display.image, VK_IMAGE_LAYOUT_GENERAL, b.image, b.newLayout,
        1, &blit, v->point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
    vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VK_CALL(vkEndCommandBuffer(v->cmd)); VK_CALL(vkResetFences(v->device, 1, &v->fence));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &v->acquired, .pWaitDstStageMask = &wait, .commandBufferCount = 1,
        .pCommandBuffers = &v->cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &v->finished[index]};
    VK_CALL(vkQueueSubmit(v->queue, 1, &submit_info, v->fence)); v->recording = 0; v->pending = 1;
    if (!n->interp) pacing_present(n->present_due, n->present_began);
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &v->finished[index], .swapchainCount = 1, .pSwapchains = &v->swapchain, .pImageIndices = &index};
    r = vkQueuePresentKHR(v->queue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) v->recreate = 1;
    else vk_require(r, "vkQueuePresentKHR");
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
void native_gpu_wait(Native *n) { (void)n; }
int native_gpu_present_filter(Native *n, int point)
{
    NativeVk *v = n->gpu; v->point = point; v->recreate = 1; return 1;
}
void native_gpu_time(Native *n, int stage, const NativeFrame *f, float t, float cpu_ms)
{ (void)n; (void)stage; (void)f; (void)t; (void)cpu_ms; }

static int native_device_supported(VkPhysicalDevice physical, VkSurfaceKHR surface, unsigned *family)
{
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(physical, &f);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(physical, &p);
    VkPhysicalDeviceFloatControlsProperties floats = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &floats};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    if (p.apiVersion < VK_API_VERSION_1_2 || !f.dualSrcBlend || !f.depthClamp ||
        !floats.shaderSignedZeroInfNanPreserveFloat32 || p.limits.maxViewportDimensions[0] < 8192 ||
        p.limits.maxViewportDimensions[1] < 8192 || p.limits.maxUniformBufferRange < sizeof(NativeConvertConstants)) return 0;
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT};
    for (unsigned j = 0; j < 2; j++) {
        VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical, formats[j], &properties);
        VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (j ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT :
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if ((properties.optimalTilingFeatures & needed) != needed) return 0;
    }
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *q = calloc(count, sizeof *q); if (!q) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, q);
    int found = 0;
    for (unsigned j = 0; j < count; j++) if ((q[j].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
        VkBool32 supported = VK_TRUE;
        if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(physical, j, surface, &supported);
        if (supported) { *family = j; found = 1; break; }
    }
    free(q); return found;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)severity; (void)type; (void)user;
    fprintf(stderr, "Vulkan validation: %s\n", data->pMessage); return VK_FALSE;
}
int gs_vulkan_native_window(Gs *g, SDL_Window *window)
{
    if (!g || !g->backend || !g->draw) return 0;
    if (native_of(g)) return 1;
    Native *n = native_create(); if (!n) return 0;
    NativeVk *v = calloc(1, sizeof *v); if (!v) { native_destroy(n); return 0; } n->gpu = v;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Downhill Revived native GS", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    const char *layer = "VK_LAYER_KHRONOS_validation", *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = validation_message};
    uint32_t extension_count = 0;
    const char *const *sdl_extensions = window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : NULL;
    if (window && !sdl_extensions) { fprintf(stderr, "gs: SDL Vulkan extensions: %s\n", SDL_GetError()); exit(1); }
    const char *extensions[32];
    if (extension_count >= 32) abort();
    for (unsigned i = 0; i < extension_count; i++) extensions[i] = sdl_extensions[i];
    if (getenv("DHD_VK_VALIDATE")) extensions[extension_count++] = extension;
    instance.enabledExtensionCount = extension_count; instance.ppEnabledExtensionNames = extensions;
    if (getenv("DHD_VK_VALIDATE")) {
        instance.enabledLayerCount = 1; instance.ppEnabledLayerNames = &layer;
        instance.pNext = &debug;
    }
    /* Explicit layer request fails if absent: validation runs cannot silently skip it. */
    VK_CALL(vkCreateInstance(&instance, NULL, &v->instance));
    if (getenv("DHD_VK_VALIDATE")) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v->instance, "vkCreateDebugUtilsMessengerEXT");
        if (!create) abort();
        VK_CALL(create(v->instance, &debug, NULL, &v->debug));
        fprintf(stderr, "gs native Vulkan: Khronos validation enabled\n");
    }
    if (window && !SDL_Vulkan_CreateSurface(window, v->instance, NULL, &v->surface)) {
        fprintf(stderr, "gs: SDL Vulkan surface: %s\n", SDL_GetError()); exit(1);
    }
    uint32_t count = 0; VK_CALL(vkEnumeratePhysicalDevices(v->instance, &count, NULL));
    VkPhysicalDevice *devices = calloc(count, sizeof *devices); if (!devices) abort();
    VK_CALL(vkEnumeratePhysicalDevices(v->instance, &count, devices));
    unsigned family = 0;
    for (unsigned j = 0; j < count; j++) if (native_device_supported(devices[j], v->surface, &family)) { v->physical = devices[j]; break; }
    free(devices);
    if (!v->physical) {
        fprintf(stderr, "gs native Vulkan: requires Vulkan 1.2 graphics/compute, dualSrcBlend, depthClamp, RGBA8 and sampled D32\n");
        if (v->debug) {
            PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v->instance, "vkDestroyDebugUtilsMessengerEXT");
            destroy(v->instance, v->debug, NULL);
        }
        vkDestroyInstance(v->instance, NULL); free(v); native_destroy(n); return 0;
    }
    v->family = family;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.dualSrcBlend = VK_TRUE, .depthClamp = VK_TRUE};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue, .pEnabledFeatures = &features};
    const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (v->surface) { device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &swapchain_extension; }
    VK_CALL(vkCreateDevice(v->physical, &device, NULL, &v->device)); vkGetDeviceQueue(v->device, family, 0, &v->queue);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(v->physical, &properties);
    v->alignment = properties.limits.minUniformBufferOffsetAlignment;
    if (v->alignment < 16) v->alignment = 16;
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VK_CALL(vkCreateCommandPool(v->device, &pool, NULL, &v->pool));
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CALL(vkAllocateCommandBuffers(v->device, &command, &v->cmd));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_CALL(vkCreateFence(v->device, &fence, NULL, &v->fence));
    VkDescriptorSetLayoutBinding bindings[21];
    for (unsigned j = 0; j < 21; j++) bindings[j] = (VkDescriptorSetLayoutBinding){.binding = descriptor_bindings[j],
        .descriptorType = descriptor_type(descriptor_bindings[j]), .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
    VkDescriptorSetLayoutCreateInfo descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 21, .pBindings = bindings};
    VK_CALL(vkCreateDescriptorSetLayout(v->device, &descriptor, NULL, &v->descriptors));
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &v->descriptors};
    VK_CALL(vkCreatePipelineLayout(v->device, &layout, NULL, &v->layout));
    enum { SETS = 16384 };
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4*SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,SETS},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,12*SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*SETS}, {VK_DESCRIPTOR_TYPE_SAMPLER,2*SETS}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SETS, .poolSizeCount = 5, .pPoolSizes = sizes};
    VK_CALL(vkCreateDescriptorPool(v->device, &dp, NULL, &v->descriptor_pool));
    for (unsigned z = 0; z < 2; z++) {
        VkAttachmentDescription attachments[2] = {
            {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
        VkAttachmentReference color = {0,VK_IMAGE_LAYOUT_GENERAL}, depth = {1,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
            .pColorAttachments = &color, .pDepthStencilAttachment = z ? &depth : NULL};
        VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = z+1, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass};
        VK_CALL(vkCreateRenderPass(v->device, &rp, NULL, &v->passes[z]));
    }
#define SHADER(index, entry) do { VkShaderModuleCreateInfo m = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, \
    .codeSize = sizeof gs_spirv_##entry, .pCode = gs_spirv_##entry}; \
    VK_CALL(vkCreateShaderModule(v->device, &m, NULL, &v->shaders[index])); } while (0)
    SHADER(0,native_vertex); SHADER(1,native_pixel); SHADER(2,native_scanout); SHADER(3,native_convert);
    SHADER(4,present_vertex); SHADER(5,present_pixel); SHADER(6,native_blit); SHADER(7,native_upload_alpha);
#undef SHADER
    for (unsigned j = 0; j < 2; j++) {
        VkComputePipelineCreateInfo c = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = v->shaders[2+j], .pName = j ? "native_convert" : "native_scanout"}, .layout = v->layout};
        VK_CALL(vkCreateComputePipelines(v->device, VK_NULL_HANDLE, 1, &c, NULL, j ? &v->convert : &v->scanout));
    }
    for (unsigned j = 0; j < 8; j++) {
        VkSamplerCreateInfo s = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = j & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = j & 2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = j & 4 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
        VK_CALL(vkCreateSampler(v->device, &s, NULL, &v->samplers[j]));
    }
    v->arena = buffer_create(v, 64u*1024*1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    v->inverse = buffer_create(v, 4096*4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); native_page_inverse(v->inverse.map);
    image_create(v, &v->dummy, 1, 1, 0); image_create(v, &v->dummy_depth, 1, 1, 1); submit(v);
    if (v->surface) {
        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CALL(vkCreateSemaphore(v->device, &sem, NULL, &v->acquired));
        int w, h; SDL_GetWindowSizeInPixels(window, &w, &h); native_vulkan_window_size(n, w, h);
    }
    native_present_options(n); native_start(g, n, (NativeHost *)g->backend);
    fprintf(stderr, "gs: Vulkan native offscreen renderer: %s\n", properties.deviceName); return 1;
}
int gs_vulkan_native(Gs *g) { return gs_vulkan_native_window(g, NULL); }

void native_vulkan_destroy(Native *n)
{
    NativeVk *v = n->gpu;
    EnterCriticalSection(&n->queue_lock); n->quit = 1; WakeAllConditionVariable(&n->queue_changed); LeaveCriticalSection(&n->queue_lock);
    if (n->thread) { WaitForSingleObject(n->thread, INFINITE); CloseHandle(n->thread); n->thread = NULL; }
    submit(v); retire(v); swapchain_free(v);
    if (v->acquired) vkDestroySemaphore(v->device, v->acquired, NULL);
    for (unsigned j = 0; j < NATIVE_TEXTURES; j++) native_gpu_texture_free(n, &n->textures[j]);
    retire(v);
    for (unsigned j = 0; j < NATIVE_SURFACES; j++) image_free(v, &v->surfaces[j]);
    for (unsigned j = 0; j < NATIVE_CONVERTS; j++) image_free(v, &v->converted[j]);
    image_free(v, &v->display); image_free(v, &v->dummy); image_free(v, &v->dummy_depth);
    buffer_free(v, &v->arena); buffer_free(v, &v->inverse); buffer_free(v, &v->readback);
    while (v->pipelines) { Pipeline *p = v->pipelines; v->pipelines = p->next; vkDestroyPipeline(v->device, p->pipeline, NULL); free(p); }
    for (unsigned j = 0; j < 8; j++) { vkDestroyShaderModule(v->device, v->shaders[j], NULL); vkDestroySampler(v->device, v->samplers[j], NULL); }
    vkDestroyPipeline(v->device, v->scanout, NULL); vkDestroyPipeline(v->device, v->convert, NULL);
    for (unsigned j = 0; j < 2; j++) vkDestroyRenderPass(v->device, v->passes[j], NULL);
    vkDestroyDescriptorPool(v->device, v->descriptor_pool, NULL); vkDestroyPipelineLayout(v->device, v->layout, NULL);
    vkDestroyDescriptorSetLayout(v->device, v->descriptors, NULL); vkDestroyFence(v->device, v->fence, NULL);
    vkDestroyCommandPool(v->device, v->pool, NULL); vkDestroyDevice(v->device, NULL);
    if (v->debug) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v->instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(v->instance, v->debug, NULL);
    }
    if (v->surface) vkDestroySurfaceKHR(v->instance, v->surface, NULL);
    vkDestroyInstance(v->instance, NULL);
    free(v); n->gpu = NULL; n->host->native = NULL;
    pthread_cond_destroy(&n->queue_changed);
    native_destroy(n);
}
