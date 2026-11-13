// Android port, Vulkan backend: presentation (see gfx_vk.h).
// With a window (the app's ANativeWindow, dm_front_window): the game's swap chain texture is scaled into a Vulkan swapchain image (aspect kept, black borders) and
// presented with FIFO (vsync) pacing. Headless (dmhost from adb shell): frames are counted, and DM_VK_SHOT=n1,n2,... writes those presented frames as PPM files to
// DM_VK_SHOT_DIR (default DM_ROOT), which is how a run without a screen is checked.
#include "gfx_vk.h"
#include <android/native_window.h>
#include <jni.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "overlay_layout.h"
#include "pixel_font.h"
#include "overlay_vert.spv.h"
#include "overlay_frag.spv.h"
#include "sharpen_vert.spv.h"
#include "sharpen_frag.spv.h"
#include "upscale_frag.spv.h"
#include "rot_vert.spv.h"
#include "rot_frag.spv.h"

__attribute__((weak)) ANativeWindow *dm_front_window(void) { return NULL; }
__attribute__((weak)) int dm_external_active(void) { return 0; }
static uint64_t g_presents;
// Vulkan present IDs include overlay redraws; only the game's tickets have a time for the NVN frame statistics.
enum { PRES_TIMES = 64 };
static struct { uint64_t ticket, ns; } g_pres_t[PRES_TIMES];
static struct { uint32_t id; uint64_t ticket; } g_pres_ids[PRES_TIMES];
static pthread_mutex_t g_pres_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pres_recorded;   // display times recorded so far (present_time_record): none after 240 presents with display timing on, the hand-over time stands in
static void present_time_begin(uint64_t ticket, uint64_t ns) {   // ns: the time to report now (no display times on this device), or 0 for the display's
    pthread_mutex_lock(&g_pres_mu);
    g_pres_t[ticket % PRES_TIMES] = (typeof(g_pres_t[0])){ticket, ns};
    pthread_mutex_unlock(&g_pres_mu);
}
static void present_time_map(uint32_t id, uint64_t ticket) {   // the present thread, under g_present_mu
    g_pres_ids[id % PRES_TIMES] = (typeof(g_pres_ids[0])){id, ticket};
}
static void present_time_record(uint32_t id, uint64_t ns) {   // the present thread, under g_present_mu
    typeof(g_pres_ids[0]) *m = &g_pres_ids[id % PRES_TIMES];
    if (m->id != id || !m->ticket || !ns) return;
    pthread_mutex_lock(&g_pres_mu);
    typeof(g_pres_t[0]) *p = &g_pres_t[m->ticket % PRES_TIMES];
    if (p->ticket == m->ticket) p->ns = ns;
    pthread_mutex_unlock(&g_pres_mu); g_pres_recorded++;
}
static void present_time_reset(void) {   // the old swapchain's pending results cannot describe the new one
    memset(g_pres_ids, 0, sizeof g_pres_ids);
    pthread_mutex_lock(&g_pres_mu); memset(g_pres_t, 0, sizeof g_pres_t); pthread_mutex_unlock(&g_pres_mu);
}
static double present_time_get(uint64_t ticket) {
    pthread_mutex_lock(&g_pres_mu);
    typeof(g_pres_t[0]) *p = &g_pres_t[ticket % PRES_TIMES];
    double t = ticket && p->ticket == ticket ? p->ns / 1e9 : 0;
    pthread_mutex_unlock(&g_pres_mu);
    return t;
}
static VkSurfaceKHR g_surf; static VkSwapchainKHR g_swap; static VkImage g_simg[8]; static uint32_t g_nsimg; static VkExtent2D g_sext; static VkFormat g_sfmt;
// The swapchain's format follows the window texture's sRGB-ness: a blit from an sRGB image into a UNORM one decodes to linear and stores that unencoded, which darkened the whole
// picture (Donkey Kong Country Returns HD presents RGBA8_SRGB, NVN format 0x38; issue #4). The Switch scans the window texture's bytes out as they are, and an SRGB -> SRGB blit
// decodes and encodes them again. UNORM games keep the UNORM swapchain. The overlay drawn after the blit is encoded too in an sRGB swapchain, so it shows a little lighter there
static VkFormat g_want = VK_FORMAT_R8G8B8A8_UNORM;
static VkSemaphore g_acq[8], g_done_sem[8]; static VkFence g_pfence[8]; static VkCommandPool g_ppool; static VkCommandBuffer g_pcb[8]; static uint32_t g_pi;
static ANativeWindow *g_win;
static VkImageView g_sview[8];
// Where the game's picture lies in the window (fractions: x, y, width, height; aspect kept, black borders around it), for the front end's touch points, which hle_touch.c
// takes relative to the picture: normalized by the whole window, a tap in Unpacking's 3040x1904 window (97 px borders) was off by up to 5% of the height
static _Atomic uint32_t g_pic[4] = {0, 0, 1u << 20, 1u << 20};
static void vk_pic_set(float x, float y, float w, float h) { atomic_store(&g_pic[0], (uint32_t)(x * (1 << 20))); atomic_store(&g_pic[1], (uint32_t)(y * (1 << 20))); atomic_store(&g_pic[2], (uint32_t)(w * (1 << 20))); atomic_store(&g_pic[3], (uint32_t)(h * (1 << 20))); }
void dm_picture_rect(float r[4]) { for (int i = 0; i < 4; i++) r[i] = (float)atomic_load(&g_pic[i]) / (1 << 20); }
__attribute__((weak)) int dm_overlay_visible(void) { return 0; }
__attribute__((weak)) float dm_overlay_scale(void) { return 1; }
__attribute__((weak)) void dm_overlay_knob(int stick, float *dx, float *dy) { (void)stick; *dx = *dy = 0; }
__attribute__((weak)) int64_t dm_overlay_pressed(int ctl, uint64_t bit) { (void)ctl; (void)bit; return 0; }
__attribute__((weak)) int dm_menu_state(void) { return -1; }
__attribute__((weak)) int dm_overlay_fps(void) { return 0; }   // front_android.c: the pause menu's Show FPS
__attribute__((weak)) int dm_save_note(char *out, int cap) { if (cap > 0) out[0] = 0; return OVX_IDLE; }   // front_android.c: the save manager's rows
__attribute__((weak)) int dm_save_rows(ov_srow rows[OVM_ROWS]) { (void)rows; return 0; }
__attribute__((weak)) int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) { (void)layout; (void)ctl; (void)x; (void)y; *s = 1; return 0; }
__attribute__((weak)) int dm_overlay_custom_ver(void) { return 0; }
__attribute__((weak)) float dm_overlay_opacity(void) { return 0.6f; }
__attribute__((weak)) int dm_overlay_layout(void) { return 0; }
__attribute__((weak)) int dm_text_entry(char *title, int tcap, char *text, int xcap, int *sel, int *shift) { (void)title; (void)tcap; (void)text; (void)xcap; (void)sel; (void)shift; return 0; }
static void pace_reset(void); static void ov_fb_drop(void);
// Pre-rotation. A landscape window on a panel whose own orientation is portrait (the Retroid Pocket 5: 1080x1920, the window 1920x1080, currentTransform
// ROTATE_90) left the turn to Android's compositor, and its display hardware does not rotate such a layer: SurfaceFlinger composited it on the GPU every
// frame ("CLIENT", "ROT_90" in its layer table) and showed a new frame only every second vsync, 30 fps on screen whatever the game made. With the surface's
// rotation as the swapchain's preTransform the images are in the panel's orientation and the display shows them as they are: the frame is composed as before
// (blit and overlay) into an image in the window's orientation (g_rot.img), and one draw (shaders/rot.*) copies it turned into the swapchain image.
// DM_VK_PRETRANSFORM=0 (at the next swapchain) leaves the turn to the compositor; it needs dynamic rendering, else the compositor turns it as before
static VkSurfaceTransformFlagBitsKHR g_pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;   // the swapchain's preTransform
static VkExtent2D g_next;   // the swapchain images' extent: the window's (g_sext), or the panel's orientation when pre-rotated
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet set;
                VkSampler samp; VkPipeline pipe; VkFormat pfmt; } g_rot;
static void rot_drop_image(void) {
    if (g_rot.view) vkDestroyImageView(V.dev, g_rot.view, NULL);
    if (g_rot.img) vkDestroyImage(V.dev, g_rot.img, NULL);
    if (g_rot.mem) vkFreeMemory(V.dev, g_rot.mem, NULL);
    g_rot.view = VK_NULL_HANDLE; g_rot.img = VK_NULL_HANDLE; g_rot.mem = VK_NULL_HANDLE;
}
static void swap_destroy(void) {
    pace_reset();
    present_time_reset();
    if (g_swap) { vkDeviceWaitIdle(V.dev); ov_fb_drop(); for (uint32_t i = 0; i < g_nsimg; i++) if (g_sview[i]) { vkDestroyImageView(V.dev, g_sview[i], NULL); g_sview[i] = VK_NULL_HANDLE; }
                  vkDestroySwapchainKHR(V.dev, g_swap, NULL); g_swap = VK_NULL_HANDLE; rot_drop_image(); g_pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; }
    if (g_surf) { vkDestroySurfaceKHR(V.inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
}
// The objects of the rotated copy: the pipeline (once per swapchain format) and the image the frame is composed into (the window's size); 0 = not available
static int rot_setup(void) {
    if (!g_rot.dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)};
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VkResult r = vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_rot.dsl);
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_rot.dsl, 1, &pr};
        if (!r) r = vkCreatePipelineLayout(V.dev, &pl, NULL, &g_rot.layout);
        if (!r) r = vkCreateDescriptorPool(V.dev, &pi, NULL, &g_rot.pool);
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_rot.pool, 1, &g_rot.dsl};
        if (!r) r = vkAllocateDescriptorSets(V.dev, &ai, &g_rot.set);
        if (!r) r = vkCreateSampler(V.dev, &si, NULL, &g_rot.samp);
        if (r) { VKRED_ONCE(110, "pre-rotation: descriptor set, layout or sampler: %d; the compositor turns the picture", r); return 0; }
    }
    if (g_rot.pfmt != g_sfmt) {
        if (g_rot.pipe) { vkDestroyPipeline(V.dev, g_rot.pipe, NULL); g_rot.pipe = VK_NULL_HANDLE; }
        VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo vc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof rot_vert_spv, rot_vert_spv};
        VkShaderModuleCreateInfo fc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof rot_frag_spv, rot_frag_spv};
        VkResult r = vkCreateShaderModule(V.dev, &vc, NULL, &vm); if (!r) r = vkCreateShaderModule(V.dev, &fc, NULL, &fm);
        VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL},
                                                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
        VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
        VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &g_sfmt};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, ss, &vi, &ia, NULL, &vs, &rs, &ms, NULL, &cbs, &dy, g_rot.layout, VK_NULL_HANDLE};
        if (!r) r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &g_rot.pipe);
        vk_pipeline_cache_changed();
        if (vm) vkDestroyShaderModule(V.dev, vm, NULL); if (fm) vkDestroyShaderModule(V.dev, fm, NULL);
        if (r) { g_rot.pipe = VK_NULL_HANDLE; VKRED_ONCE(111, "pre-rotation: pipeline for format %d: %d; the compositor turns the picture", g_sfmt, r); return 0; }
        g_rot.pfmt = g_sfmt;
    }
    rot_drop_image();
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, g_sfmt, {g_sext.width, g_sext.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(V.dev, &ic, NULL, &g_rot.img);
    VkMemoryRequirements rq = {0}; if (!r) vkGetImageMemoryRequirements(V.dev, g_rot.img, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r) r = vkAllocateMemory(V.dev, &ai, NULL, &g_rot.mem);
    if (!r) r = vkBindImageMemory(V.dev, g_rot.img, g_rot.mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_rot.img, VK_IMAGE_VIEW_TYPE_2D, g_sfmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vkCreateImageView(V.dev, &vi, NULL, &g_rot.view);
    if (r) { rot_drop_image(); VKRED_ONCE(112, "pre-rotation: the %ux%u image: %d; the compositor turns the picture", g_sext.width, g_sext.height, r); return 0; }
    VkDescriptorImageInfo ii = {g_rot.samp, g_rot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_rot.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
    vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
    return 1;
}
static int swap_make(void) {
    ANativeWindow *w = dm_front_window();
    if (w != g_win) { swap_destroy(); g_win = w; }
    if (!w) return 0;
    int ww = ANativeWindow_getWidth(w), wh = ANativeWindow_getHeight(w);
    if (g_swap && ww > 0 && wh > 0 && ((uint32_t)ww != g_sext.width || (uint32_t)wh != g_sext.height)) swap_destroy();
    if (g_swap) return 1;
    VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, w};
    if (vkCreateAndroidSurfaceKHR(V.inst, &si, NULL, &g_surf)) return 0;
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, g_surf, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, sf);
    g_sfmt = sf[0].format; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM) g_sfmt = sf[i].format;
    for (uint32_t i = 0; i < nf; i++) if (sf[i].format == g_want) g_sfmt = g_want;   // the window texture's sRGB-ness (g_want), so the blit copies its bytes as they are
    if (g_sfmt != g_want) VKRED_ONCE(95, "the surface offers no format %d for the window texture: its picture is %s", g_want, g_want == VK_FORMAT_R8G8B8A8_SRGB ? "darker" : "lighter");
    g_sext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(w), (uint32_t)ANativeWindow_getHeight(w)};   // the window as it is now; Android's compositor rotates (identity transform)
    if (!g_sext.width || !g_sext.height) g_sext = caps.currentExtent;
    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    int want = 1; { const char *e = getenv("DM_VK_PRETRANSFORM"); if (e && *e) want = atoi(e) != 0; }
    VkSurfaceTransformFlagBitsKHR turn = (VkSurfaceTransformFlagBitsKHR)(caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR));
    g_pre = want && turn && (caps.supportedTransforms & turn) && V.has_dynrender && rot_setup() ? turn : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    g_next = g_pre == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? g_sext : (VkExtent2D){g_sext.height, g_sext.width};
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, g_surf, n, g_sfmt, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, g_next, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, g_pre,
        (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, 1, VK_NULL_HANDLE};
    VkResult r = vkCreateSwapchainKHR(V.dev, &ci, NULL, &g_swap);
    if (r) { VKLOG("[RED] swapchain creation failed: %d", r); swap_destroy(); return 0; }
    g_nsimg = 8; vkGetSwapchainImagesKHR(V.dev, g_swap, &g_nsimg, g_simg);
    for (uint32_t i = 0; i < g_nsimg; i++) { VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_simg[i], VK_IMAGE_VIEW_TYPE_2D, g_sfmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHK(vkCreateImageView(V.dev, &vi, NULL, &g_sview[i])); }
    if (!g_ppool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf}; VKCHK(vkCreateCommandPool(V.dev, &pci, NULL, &g_ppool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_ppool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8}; VKCHK(vkAllocateCommandBuffers(V.dev, &ai, g_pcb));
        for (int i = 0; i < 8; i++) {
            VkSemaphoreCreateInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHK(vkCreateSemaphore(V.dev, &s, NULL, &g_acq[i])); VKCHK(vkCreateSemaphore(V.dev, &s, NULL, &g_done_sem[i]));
            VkFenceCreateInfo f = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT}; VKCHK(vkCreateFence(V.dev, &f, NULL, &g_pfence[i]));
        }
    }
    if (g_pre != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) VKLOG("swapchain %ux%u format %d, %u images, pre-rotated (transform %#x; the window %ux%u)", g_next.width, g_next.height, g_sfmt, g_nsimg, (unsigned)g_pre, g_sext.width, g_sext.height);
    else VKLOG("swapchain %ux%u format %d, %u images%s", g_sext.width, g_sext.height, g_sfmt, g_nsimg, turn ? " (the compositor turns the picture)" : "");
    return 1;
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
// ---- the on-screen gamepad: rasterized on the CPU once per window size (half resolution), blended over each frame ----
// The texture is the static layer (w x h), a strip of sprites under it (sh rows) and the pause menu's panel under that (pw x ph). The sprites: each stick's
// knob, drawn every frame where the finger holds it; the menu button, drawn on its own at OV_MENU_ALPHA whether the gamepad is shown or not; a white disc, the
// glow over a pressed round button (a wider one glows in its own shape); a black block, the dimming behind the menu. sp: each sprite in the strip (centre x, y,
// half width and height, texels; 0 = none). The panel is rasterized again whenever the menu's state (dm_menu_state) changes. Under the panel, the frame rate
// counter's characters (OV_FPS_CHARS, gh rows: one cell each), from which the counter is put together on every present, so a changing number needs no upload.
// With a theme in the APK (overlay_theme.c), the gamepad is its sprites instead: an atlas per layout in a texture of its own (g_tt), uploaded once, every
// control drawn from it on each present; the procedural layer is then only the menu's panel, the dimming and the counter.
enum { SP_DISC = OV_N, SP_BLACK, SP_WHITE, SP_ACCENT, SP_N };   // (black, white, accent: solid blocks, the dimming and the layout editor's frames)
#define OV_FPS_CHARS "0123456789 FPS-CBATO"   // (the temperature rows: "SOC 52C", "BAT 38C")
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler samp; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet set;
                VkPipeline pipe; VkFormat pfmt; uint32_t w, h, sh, pw, ph, gh, gs, tw, py; VkBuffer up; VkDeviceMemory upm; uint8_t *px; int uploaded, panel, lay, cver; float sp[SP_N][4];
                VkRenderPass rp; VkFramebuffer fb[8]; VkImageView fbview[8]; VkExtent2D fbext[8]; } g_ov;
static struct { const ov_theme *t; VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet set; VkBuffer up; VkDeviceMemory upm; uint32_t upk; int ready; } g_tt[2];   // per layout
// Without dynamic rendering (a Vulkan 1.1 / 1.2 device, V.has_dynrender) the overlay draws in a render pass object over the swapchain image: one pass for the swapchain's
// format (load and store, the image already in COLOR_ATTACHMENT_OPTIMAL), and a framebuffer per swapchain image, made again when its view or the extent changes
static VkRenderPass ov_rp(void) {
    static VkFormat fmt; if (g_ov.rp && fmt == g_sfmt) return g_ov.rp;
    if (g_ov.rp) { vkDestroyRenderPass(V.dev, g_ov.rp, NULL); g_ov.rp = VK_NULL_HANDLE; }   // the swapchain's format changed (g_want); swap_destroy dropped its framebuffers
    fmt = g_sfmt;
    VkAttachmentDescription a = {0, g_sfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference r = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &r, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &a, 1, &sp, 0, NULL};
    if (vkCreateRenderPass(V.dev, &ci, NULL, &g_ov.rp)) { VKRED_ONCE(93, "overlay render pass failed"); g_ov.rp = VK_NULL_HANDLE; }
    return g_ov.rp;
}
// every overlay framebuffer goes with the views or the pass it was made for: a view handle made again later may have the same value (ov_fb would take it for its own)
static void ov_fb_drop(void) { for (int i = 0; i < 8; i++) { if (g_ov.fb[i]) vkDestroyFramebuffer(V.dev, g_ov.fb[i], NULL); g_ov.fb[i] = VK_NULL_HANDLE; g_ov.fbview[i] = VK_NULL_HANDLE; g_ov.fbext[i] = (VkExtent2D){0, 0}; } }
static VkFramebuffer ov_fb(uint32_t idx) {
    if (idx >= 8 || !ov_rp()) return VK_NULL_HANDLE;
    if (g_ov.fb[idx] && g_ov.fbview[idx] == g_sview[idx] && g_ov.fbext[idx].width == g_sext.width && g_ov.fbext[idx].height == g_sext.height) return g_ov.fb[idx];
    if (g_ov.fb[idx]) vkDestroyFramebuffer(V.dev, g_ov.fb[idx], NULL);   // (swap_destroy drops them all with the views; here only an extent change)
    VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_ov.rp, 1, &g_sview[idx], g_sext.width, g_sext.height, 1};
    if (vkCreateFramebuffer(V.dev, &ci, NULL, &g_ov.fb[idx])) { VKRED_ONCE(94, "overlay framebuffer failed"); g_ov.fb[idx] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    g_ov.fbview[idx] = g_sview[idx]; g_ov.fbext[idx] = g_sext;
    return g_ov.fb[idx];
}
static _Atomic uint64_t g_fps_frames;   // the game's frames shown (mtl_present), which the counter counts
static int ov_round(const ov_ctl *c) { return c->w == c->h; }
static uint32_t ov_layout_sprites(uint32_t w, uint32_t h) {   // places the sprites, returns the strip's height
    float x = 4, top = (float)h, mr = 0, disc = 0, b[4];
    for (int i = 0; i < OV_N; i++) { ov_box(&OV_CTLS[i], (float)w, (float)h, b); if (OV_CTLS[i].kind == OV_BUTTON && ov_round(&OV_CTLS[i]) && b[2] > disc) disc = b[2]; }
    for (int i = 0; i < SP_N; i++) {
        const ov_ctl *c = i < OV_N ? &OV_CTLS[i] : NULL; float hw = 0, hh = 0;
        if (!c) hw = hh = i == SP_DISC ? disc : 6;
        else { ov_box(c, (float)w, (float)h, b);
            if (c->kind == OV_STICK_L || c->kind == OV_STICK_R) hw = hh = b[2] * OV_KNOB;
            else if (c->kind == OV_MENU || (c->kind == OV_BUTTON && !ov_round(c))) { hw = b[2]; hh = b[3]; } }   // a wider button's glow is its own shape
        g_ov.sp[i][2] = hw; g_ov.sp[i][3] = hh; if (!hw) continue; g_ov.sp[i][0] = x + hw + 2; x += 2 * hw + 8; if (hh > mr) mr = hh;
    }
    for (int i = 0; i < SP_N; i++) g_ov.sp[i][1] = top + mr + 4;
    return (uint32_t)(2 * mr + 8);
}
static void ov_plot(uint8_t *px, uint32_t w, uint32_t h, int x, int y, uint8_t a, uint8_t v) {
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;
    uint8_t *p = px + 4 * ((size_t)y * w + (size_t)x); if (p[3] >= a) return;
    p[0] = p[1] = p[2] = v; p[3] = a;
}
static void ov_circle(uint8_t *px, uint32_t w, uint32_t h, float cx, float cy, float r, float ring) {
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++) for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (d <= r && d >= r - ring) ov_plot(px, w, h, x, y, 150, 255);   // the ring
        else if (d < r - ring) ov_plot(px, w, h, x, y, 50, 30);           // a dark, see-through inside
    }
}
static void ov_box_ring(uint8_t *px, uint32_t w, uint32_t h, const ov_ctl *c, const float b[4], float ring) {   // ov_circle for any control's shape (b: ov_box)
    for (int y = (int)(b[1] - b[3] - 2); y <= (int)(b[1] + b[3] + 2); y++) for (int x = (int)(b[0] - b[2] - 2); x <= (int)(b[0] + b[2] + 2); x++) {
        float d = ov_sdf(c, b, x - b[0], y - b[1]);
        if (d <= 0 && d >= -ring) ov_plot(px, w, h, x, y, 150, 255);
        else if (d < -ring) ov_plot(px, w, h, x, y, 50, 30);
    }
}
const uint8_t *dm_overlay_font(void);   // overlay_theme.c
typedef struct { uint8_t *px; uint32_t w, h; } ov_plain;
static void ov_text_plot(void *u, int x, int y) { const ov_plain *o = u; ov_plot(o->px, o->w, o->h, x, y, 220, 255); }
static void ov_text(uint8_t *px, uint32_t w, uint32_t h, float cx, float cy, const char *s, int scale) {
    const uint8_t *f = dm_overlay_font(); ov_plain o = {px, w, h};
    pf_draw(f, s, (int)(cx - pf_width(f, s) * scale / 2.0f), (int)(cy + PF_CAP * scale / 2.0f), scale, ov_text_plot, &o);
}
// colour drawing for the menu button and the panel: straight alpha, "over" blending, shapes antialiased by their distance
typedef struct { uint8_t *px; uint32_t stride, x0, y0, w, h; } ov_canvas;   // a w x h window of rows `stride` texels wide, at (x0, y0)
static void cv_blend(const ov_canvas *cv, int x, int y, const uint8_t c[4], float cov) {
    if (x < 0 || y < 0 || (uint32_t)x >= cv->w || (uint32_t)y >= cv->h || cov <= 0) return;
    uint8_t *p = cv->px + 4 * ((size_t)(cv->y0 + y) * cv->stride + cv->x0 + x);
    float a = c[3] / 255.0f * (cov > 1 ? 1 : cov), da = p[3] / 255.0f, oa = a + da * (1 - a);
    if (oa <= 0) return;
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)((c[k] * a + p[k] * da * (1 - a)) / oa + 0.5f);
    p[3] = (uint8_t)(oa * 255 + 0.5f);
}
static void cv_rrect(const ov_canvas *cv, float x0, float y0, float x1, float y1, float rad, const uint8_t c[4]) {   // a filled rounded rectangle
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - rad, hy = (y1 - y0) / 2 - rad;
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++) for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
        float qx = fabsf(x + 0.5f - cx) - hx, qy = fabsf(y + 0.5f - cy) - hy, ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
        cv_blend(cv, x, y, c, 0.5f - d);
    }
}
static void cv_disc(const ov_canvas *cv, float cx, float cy, float r, const uint8_t c[4]) { cv_rrect(cv, cx - r, cy - r, cx + r, cy + r, r, c); }
static void cv_box(const ov_canvas *cv, const ov_ctl *c, const float b[4], float inset, const uint8_t col[4]) {   // a control's shape filled (b: ov_box), inset texels in
    for (int y = (int)(b[1] - b[3]) - 1; y <= (int)(b[1] + b[3]) + 1; y++) for (int x = (int)(b[0] - b[2]) - 1; x <= (int)(b[0] + b[2]) + 1; x++)
        cv_blend(cv, x, y, col, 0.5f - (ov_sdf(c, b, x + 0.5f - b[0], y + 0.5f - b[1]) + inset));
}
static void cv_ring(const ov_canvas *cv, float cx, float cy, float r, float t, const uint8_t c[4]) {
    for (int y = (int)(cy - r) - 1; y <= (int)(cy + r) + 1; y++) for (int x = (int)(cx - r) - 1; x <= (int)(cx + r) + 1; x++) {
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
        cv_blend(cv, x, y, c, fminf(0.5f - (d - r), 0.5f + (d - (r - t))));
    }
}
static float cv_text_w(const char *s, int scale) { return (float)(pf_width(dm_overlay_font(), s) * scale); }
typedef struct { const ov_canvas *cv; const uint8_t *c; int base; float slant; } cv_ink;   // slant: texels right per texel above the baseline
static void cv_text_plot(void *u, int x, int y) { const cv_ink *k = u; cv_blend(k->cv, x + (int)floorf((k->base - y) * k->slant), y, k->c, 1); }
static void cv_text_at(const ov_canvas *cv, float x, float cy, const char *s, int scale, const uint8_t c[4], float slant) {   // x: the left edge
    int base = (int)(cy + PF_CAP * scale / 2.0f + 0.5f); cv_ink k = {cv, c, base, slant}; pf_draw(dm_overlay_font(), s, (int)(x + 0.5f), base, scale, cv_text_plot, &k);
}
static void cv_text(const ov_canvas *cv, float x, float cy, const char *s, int scale, const uint8_t c[4]) { cv_text_at(cv, x, cy, s, scale, c, 0); }
// ---- the shell (pause menu, Remap buttons, save picker, text entry, layout editor): drawn 1:1 into a raster the window's size. Its look: a near-black panel
// docked on the left with a slanted edge and an accent line, rounded cards, one accent (green) and greys, Roboto (a smooth font, below) ----
static const uint8_t C_WHITE[4] = {255, 255, 255, 255}, C_ACCENT[4] = {43, 224, 107, 255}, C_HOT[4] = {255, 64, 48, 255};
static const uint8_t M_ACC_D[4] = {20, 168, 76, 255}, M_PANEL[4] = {13, 13, 15, 236}, M_CARD[4] = {22, 22, 25, 218}, M_CARD_HI[4] = {34, 36, 38, 230},
                     M_TXT[4] = {214, 214, 219, 255}, M_SUB[4] = {150, 150, 158, 255}, M_DARK[4] = {10, 12, 11, 255}, M_FAINT[4] = {255, 255, 255, 34},
                     M_ROWHI[4] = {255, 255, 255, 14};
static void cv_fill(const ov_canvas *cv, float x0, float y0, float x1, float y1, const uint8_t c[4]) { cv_rrect(cv, x0, y0, x1, y1, 0, c); }
static const char *cv_caps(const char *s, char *b, size_t n) {   // ASCII letters in capitals (other scripts as they are)
    size_t i = 0; for (; s[i] && i + 1 < n; i++) b[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    b[i] = 0; return b;
}

// The smooth font (tools/ui_atlas.py: Roboto regular, medium and bold italic baked at a few sizes, assets/font/ui_smooth.bin). A glyph is drawn from the next
// larger baked size, area-averaged down; a character the atlas lacks (or every one, without the atlas) comes from the pixel font
enum { SF_REG, SF_MED, SF_BOLDI };
typedef struct __attribute__((packed)) { uint32_t cp; uint16_t x, y, w, h; int16_t l, t; uint16_t adv; } sf_glyph;
typedef struct __attribute__((packed)) { int16_t asc, desc; uint32_t first, n; } sf_set;
static struct { int ok, faces, nsz, aw, ah; const uint16_t *sz; const sf_set *set; const sf_glyph *g; const uint8_t *px; } SF;
__attribute__((weak)) const uint8_t *dm_overlay_sfont(size_t *n) { *n = 0; return NULL; }   // (overlay_theme.c)
static int sf_init(void) {
    static int tried; if (tried) return SF.ok; tried = 1;
    size_t n = 0; const uint8_t *b = dm_overlay_sfont(&n); if (!b || n < 16 || memcmp(b, "NXSF", 4)) return 0;
    uint16_t h[4]; memcpy(h, b + 8, 8); SF.faces = h[0]; SF.nsz = h[1]; SF.aw = h[2]; SF.ah = h[3];
    size_t o = 16; SF.sz = (const uint16_t *)(b + o); o += 2u * SF.nsz; SF.set = (const sf_set *)(b + o); o += sizeof(sf_set) * SF.faces * SF.nsz;
    if (o > n) return 0;
    uint32_t ng = 0; for (int i = 0; i < SF.faces * SF.nsz; i++) ng += SF.set[i].n;
    SF.g = (const sf_glyph *)(b + o); o += sizeof(sf_glyph) * ng; SF.px = b + o;
    if (SF.faces < 3 || o + (size_t)SF.aw * SF.ah > n) return 0;
    return SF.ok = 1;
}
static const sf_set *sf_pick(int face, float px, float *k) {   // the set to draw a face at px from, and the scale (the atlas's sizes per face, ascending; 0 = none)
    if (!sf_init()) return NULL;
    const uint16_t *sz = SF.sz; int i = 0, last = -1;
    for (int j = 0; j < SF.nsz; j++) if (SF.set[face * SF.nsz + j].n) { last = j; if (sz[j] >= px) { i = j; break; } i = j; }
    if (last < 0) return NULL;
    *k = px / sz[i]; return &SF.set[face * SF.nsz + i];
}
static const sf_glyph *sf_find(const sf_set *s, uint32_t cp) {
    const sf_glyph *g = SF.g + s->first; int lo = 0, hi = (int)s->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g[m].cp == cp) return &g[m]; if (g[m].cp < cp) lo = m + 1; else hi = m - 1; }
    return NULL;
}
static void cv_sglyph(const ov_canvas *cv, const sf_glyph *g, float x0, float y0, float k, const uint8_t c[4]) {   // area-averaged to scale k, top left (x0, y0)
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x0 + g->w * k), iy1 = (int)ceilf(y0 + g->h * k); float norm = k * k;
    for (int iy = iy0; iy < iy1; iy++) {
        float sy0 = (iy - y0) / k, sy1 = (iy + 1 - y0) / k;
        for (int ix = ix0; ix < ix1; ix++) {
            float sx0 = (ix - x0) / k, sx1 = (ix + 1 - x0) / k, acc = 0;
            for (int sy = (int)floorf(fmaxf(sy0, 0)); sy < g->h && sy < sy1; sy++) {
                float wy = fminf(sy1, sy + 1) - fmaxf(sy0, sy); if (wy <= 0) continue;
                const uint8_t *row = SF.px + (size_t)(g->y + sy) * SF.aw + g->x;
                for (int sx = (int)floorf(fmaxf(sx0, 0)); sx < g->w && sx < sx1; sx++) { float wx = fminf(sx1, sx + 1) - fmaxf(sx0, sx); if (wx > 0) acc += row[sx] * wx * wy; }
            }
            if (acc > 0) cv_blend(cv, ix, iy, c, acc / 255.0f * norm);
        }
    }
}
// a line of text from x on the baseline base, px its em; track: added after each character; cv NULL only measures. Returns its width
static float cv_s(const ov_canvas *cv, float x, float base, const char *s, int face, float px, float track, const uint8_t c[4]) {
    float k = 1, x0 = x; const sf_set *set = sf_pick(face, px, &k);
    while (*s) {
        const char *p0 = s; uint32_t cp = pf_next(&s); const sf_glyph *g = set ? sf_find(set, cp) : NULL;
        if (g) { if (cv && g->w) cv_sglyph(cv, g, x + g->l * k, base + g->t * k, k, c); x += g->adv / 64.0f * k + track; continue; }
        char one[8] = {0}; memcpy(one, p0, (size_t)(s - p0) < 7 ? (size_t)(s - p0) : 7);   // the pixel font's glyph, about the cap height
        int ps = (int)fmaxf(1, roundf(px * 0.72f / PF_CAP));
        if (cv) { cv_ink ink = {cv, c, (int)base, 0}; pf_draw(dm_overlay_font(), one, (int)(x + 0.5f), (int)(base + 0.5f), ps, cv_text_plot, &ink); }
        x += pf_width(dm_overlay_font(), one) * ps + track;
    }
    return x - x0;
}
static float cv_sw(const char *s, int face, float px) { return cv_s(NULL, 0, 0, s, face, px, 0, NULL); }
static float cv_sc(const ov_canvas *cv, float x, float cy, const char *s, int face, float px, float track, const uint8_t c[4]) {   // centred on cy (the cap height's middle)
    return cv_s(cv, x, cy + 0.355f * px, s, face, px, track, c);
}
static void cv_sfit(char *s, int face, float px, float room) { while (s[0] && cv_sw(s, face, px) > room) pf_chop(s, strlen(s)); }   // cut until it fits
// a text wrapped to width w from its top y (lines 1.4 em apart): at spaces or after a slash, a word wider than a line by its characters; cv NULL only measures. Returns its height
static float cv_swrap(const ov_canvas *cv, float x, float y, float w, const char *s, int face, float px, const uint8_t c[4]) {
    float lh = 1.4f * px, top = y; char line[400];
    while (*s) {
        size_t n = 0, fit = 0, sp = 0; const char *p = s;
        while (*p && n < sizeof line - 8) {   // the longest start of s that fits; fit: up to its last space
            const char *q = p; pf_next(&q); memcpy(line + n, p, (size_t)(q - p)); line[n + (q - p)] = 0;
            if (cv_sw(line, face, px) > w && n) break;
            n += (size_t)(q - p); p = q; if (*p == ' ') { fit = n; sp = 1; } else if (line[n - 1] == '/') { fit = n; sp = 0; }   // (a path: after a slash)
        }
        if (*p && fit) { n = fit; p = s + fit + sp; }   // break there
        line[n] = 0; if (cv) cv_s(cv, x, y + 0.75f * lh, line, face, px, 0, c);
        y += lh; s = p; while (*s == ' ') s++;
    }
    return y - top;
}

// shapes, antialiased by their distance
static float cv_box_d(float px, float py, float cx, float cy, float hx, float hy, float rad) {   // a rounded box's signed distance
    float qx = fabsf(px - cx) - (hx - rad), qy = fabsf(py - cy) - (hy - rad), ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
}
static void cv_rring(const ov_canvas *cv, float x0, float y0, float x1, float y1, float rad, float t, const uint8_t c[4]) {   // a rounded rectangle's outline, t inside
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++) for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
        float d = cv_box_d(x + 0.5f, y + 0.5f, cx, cy, hx, hy, rad); cv_blend(cv, x, y, c, fminf(0.5f - d, 0.5f + d + t));
    }
}
static void cv_line(const ov_canvas *cv, float ax, float ay, float bx, float by, float wd, const uint8_t c[4]) {   // a stroke with round ends
    float r = wd / 2, dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    for (int y = (int)(fminf(ay, by) - r) - 1; y <= (int)(fmaxf(ay, by) + r) + 1; y++) for (int x = (int)(fminf(ax, bx) - r) - 1; x <= (int)(fmaxf(ax, bx) + r) + 1; x++) {
        float px = x + 0.5f - ax, py = y + 0.5f - ay, t = l2 > 0 ? fminf(fmaxf((px * dx + py * dy) / l2, 0), 1) : 0, ex = px - t * dx, ey = py - t * dy;
        cv_blend(cv, x, y, c, 0.5f - (sqrtf(ex * ex + ey * ey) - r));
    }
}
static void cv_tri(const ov_canvas *cv, float ax, float ay, float bx, float by, float qx, float qy, const uint8_t c[4]) {   // a filled triangle, 4x4 samples a texel
    for (int y = (int)fminf(ay, fminf(by, qy)) - 1; y <= (int)fmaxf(ay, fmaxf(by, qy)) + 1; y++) for (int x = (int)fminf(ax, fminf(bx, qx)) - 1; x <= (int)fmaxf(ax, fmaxf(bx, qx)) + 1; x++) {
        int in = 0;
        for (int s = 0; s < 16; s++) { float px = x + (s % 4 + 0.5f) / 4, py = y + (s / 4 + 0.5f) / 4;
            float e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax), e1 = (qx - bx) * (py - by) - (qy - by) * (px - bx), e2 = (ax - qx) * (py - qy) - (ay - qy) * (px - qx);
            in += (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0); }
        cv_blend(cv, x, y, c, in / 16.0f);
    }
}
static void cv_mix(uint8_t o[4], const uint8_t a[4], const uint8_t b[4], float t) { for (int i = 0; i < 4; i++) o[i] = (uint8_t)(a[i] + (b[i] - a[i]) * t + 0.5f); }

// the menu button without a theme: a dark disc and three bars
static void ov_menu_icon(const ov_canvas *cv, const ov_ctl *c, const float b[4]) {
    static const uint8_t fill[4] = {24, 24, 27, 215}; (void)c;
    float r = fminf(b[2], b[3]), lw = fmaxf(2, r * 0.10f), bw = r * 0.40f;
    cv_disc(cv, b[0], b[1], r, fill); cv_ring(cv, b[0], b[1], r, fmaxf(1, r * 0.04f), M_FAINT);
    for (int k = -1; k <= 1; k++) cv_line(cv, b[0] - bw, b[1] + k * r * 0.26f, b[0] + bw, b[1] + k * r * 0.26f, lw, C_WHITE);
}
// a rounded card: lighter and with a full accent line when chosen
static void ov_m_cardbg(const ov_canvas *cv, float x0, float y0, float x1, float y1, float u, int on) {
    float rad = 0.011f * u; uint8_t edge[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], (uint8_t)(on ? 255 : 90)};
    cv_rrect(cv, x0, y0, x1, y1, rad, on ? M_CARD_HI : M_CARD); cv_rring(cv, x0, y0, x1, y1, rad, on ? 2 : 1, edge);
}
// a button: an accent outline and its word; filled (the default) or held: the accent with dark words
static void ov_m_button(const ov_canvas *cv, float x0, float y0, float x1, float y1, const char *l, float px, int fill, int held) {
    float h = y1 - y0, r = h * 0.22f; uint8_t tint[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], 40};
    if (fill || held) cv_rrect(cv, x0, y0, x1, y1, r, held ? C_WHITE : C_ACCENT);
    else { cv_rrect(cv, x0, y0, x1, y1, r, tint); cv_rring(cv, x0, y0, x1, y1, r, 1.5f, C_ACCENT); }
    while (px > 6 && cv_sw(l, SF_MED, px) > (x1 - x0) * 0.86f) px *= 0.94f;
    cv_sc(cv, (x0 + x1) / 2 - cv_sw(l, SF_MED, px) / 2, (y0 + y1) / 2, l, SF_MED, px, 0, fill || held ? M_DARK : C_WHITE);
}
// segments in a rounded group, the current one filled with the accent
static void ov_m_segments(const ov_canvas *cv, float x0, float x1, float cy, float hh, const char *const *seg, int n, int cur, float px) {
    float sw = (x1 - x0) / n, y0 = cy - hh / 2, y1 = cy + hh / 2, r = hh * 0.22f;
    cv_rrect(cv, x0, y0, x1, y1, r, M_FAINT);
    cv_rrect(cv, x0 + cur * sw + 2, y0 + 2, x0 + (cur + 1) * sw - 2, y1 - 2, r - 1, C_ACCENT);
    for (int k = 0; k < n; k++) cv_sc(cv, x0 + k * sw + (sw - cv_sw(seg[k], SF_MED, px)) / 2, cy, seg[k], SF_MED, px, 0, k == cur ? M_DARK : M_TXT);
}
// the line icons (a box of size s centred on cx, cy; stroke lw)
static void ov_m_icon(const ov_canvas *cv, int i, float cx, float cy, float s, float lw, const uint8_t c[4]) {
    if (i == 0) {   // a controller: its body, a d-pad, two buttons
        float hx = s * 0.50f, hy = s * 0.30f; cv_rring(cv, cx - hx, cy - hy, cx + hx, cy + hy, hy * 0.95f, lw, c);
        float dx = cx - s * 0.23f, a = s * 0.11f; cv_line(cv, dx - a, cy, dx + a, cy, lw, c); cv_line(cv, dx, cy - a, dx, cy + a, lw, c);
        cv_disc(cv, cx + s * 0.17f, cy - s * 0.04f, lw * 0.85f, c); cv_disc(cv, cx + s * 0.29f, cy + s * 0.06f, lw * 0.85f, c);
    } else if (i == 1) {   // three sliders
        static const float kx[3] = {-0.18f, 0.20f, -0.02f};
        for (int k = 0; k < 3; k++) { float y = cy + (k - 1) * s * 0.30f, x = cx + kx[k] * s; cv_line(cv, cx - s * 0.45f, y, cx + s * 0.45f, y, lw, c);
            cv_disc(cv, x, y, s * 0.10f + lw * 0.2f, M_PANEL); cv_ring(cv, x, y, s * 0.10f + lw * 0.5f, lw, c); }
    } else {   // a monitor
        float hx = s * 0.48f, y0 = cy - s * 0.36f, y1 = cy + s * 0.20f; cv_rring(cv, cx - hx, y0, cx + hx, y1, s * 0.06f, lw, c);
        cv_line(cv, cx, y1, cx, cy + s * 0.38f, lw, c); cv_line(cv, cx - s * 0.20f, cy + s * 0.38f, cx + s * 0.20f, cy + s * 0.38f, lw, c);
    }
}
// the panel on the left: its ground, its edge's accent line, the header (the game's name, its last word in the accent; "Paused"), the controller's keys at its foot
__attribute__((weak)) const char *dm_game_name(void) { return ""; }   // (front_android.c: nx2a-data.json's name)
static void ov_m_panel(const ov_canvas *cv, const char *const keys[3][2]) {
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H);
    for (int y = 0; y < (int)H; y++) { float e = ov_m_edge(W, H, y + 0.5f);
        for (int x = 0; x <= (int)e + 1; x++) cv_blend(cv, x, y, M_PANEL, fminf(1, e - x));
        for (int x = (int)e - 2; x <= (int)e + 2; x++) cv_blend(cv, x, y, C_ACCENT, fminf(fminf(x + 1 - e, e + 2 - x), 1) * 0.9f); }
    float y0 = 0.035f * u, y1 = 0.185f * u, xe = ov_m_edge(W, H, y0);
    for (int y = (int)y0; y < (int)y1; y++) { float e = ov_m_edge(W, H, (float)y); for (int x = 0; x < (int)e; x++) {   // the accent's tint, fading to the right
        uint8_t t[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], (uint8_t)(70 * (1 - x / e) * (1 - x / e) + 12)}; cv_blend(cv, x, y, t, fminf(1, e - x)); } }
    cv_fill(cv, 0, y0, 0.0067f * u, y1, C_ACCENT);
    float bs = 0.085f * u, bx = 0.055f * u, by = (y0 + y1) / 2 - bs / 2; static const uint8_t box[4] = {28, 29, 32, 255};
    cv_rrect(cv, bx, by, bx + bs, by + bs, bs * 0.18f, box); cv_rring(cv, bx, by, bx + bs, by + bs, bs * 0.18f, 1, M_FAINT);
    float icx = bx + bs / 2, icy = by + bs / 2; cv_ring(cv, icx, icy, bs * 0.30f, fmaxf(1.5f, 0.0028f * u), C_ACCENT);
    cv_fill(cv, icx - bs * 0.09f, icy - bs * 0.11f, icx - bs * 0.04f, icy + bs * 0.11f, C_WHITE); cv_fill(cv, icx + bs * 0.04f, icy - bs * 0.11f, icx + bs * 0.09f, icy + bs * 0.11f, C_WHITE);
    char up[96]; cv_caps(dm_game_name(), up, sizeof up); char *sp = strrchr(up, ' ');
    float tx = bx + bs + 0.028f * u, tpx = 0.036f * u, room = xe - tx - 0.03f * u, ty = (y0 + y1) / 2 - 0.012f * u;
    while (tpx > 0.02f * u && cv_sw(up, SF_BOLDI, tpx) > room) tpx *= 0.94f;
    cv_sfit(up, SF_BOLDI, tpx, room); sp = strrchr(up, ' ');
    if (sp) { *sp = 0; float w = cv_s(cv, tx, ty, up, SF_BOLDI, tpx, 0, C_WHITE) + cv_sw(" ", SF_BOLDI, tpx); cv_s(cv, tx + w, ty, sp + 1, SF_BOLDI, tpx, 0, C_ACCENT); }
    else cv_s(cv, tx, ty, up, SF_BOLDI, tpx, 0, C_WHITE);
    float sy = ty + 0.036f * u; cv_disc(cv, tx + 0.008f * u, sy - 0.008f * u, 0.0075f * u, C_ACCENT); cv_s(cv, tx + 0.026f * u, sy, "Paused", SF_REG, 0.025f * u, 0, M_TXT);
    float x = 0.055f * u, y = H - 0.06f * u, kp = 0.020f * u;
    for (int k = 0; k < 3 && keys[k][0]; k++) { float kw = cv_sw(keys[k][0], SF_MED, kp) + 0.022f * u;
        cv_rring(cv, x, y - 0.019f * u, x + kw, y + 0.019f * u, 0.008f * u, 1, M_SUB); cv_sc(cv, x + 0.011f * u, y, keys[k][0], SF_MED, kp, 0, M_TXT);
        x += kw + 0.012f * u; x += cv_sc(cv, x, y, keys[k][1], SF_REG, kp, 0, M_SUB) + 0.035f * u; }
}
// the main button (Resume, Back, Cancel): a parallelogram in the accent's gradient, a soft glow, two stripes after it; a controller's highlight: a white outline
static void ov_m_main(const ov_canvas *cv, const char *word, int focus) {
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H), r[4]; ov_m_card(W, H, OVM_ROWS - 1, r);
    float x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3], cy = (y0 + y1) / 2, sk = 0.22f, rad = 0.012f * u, gl = 0.03f * u;
    for (int y = (int)(y0 - gl); y <= (int)(y1 + gl); y++) for (int x = (int)(x0 - gl - r[3]); x <= (int)(x1 + gl + r[3]); x++) {
        float px = x + 0.5f + (y + 0.5f - cy) * sk, d = cv_box_d(px, y + 0.5f, (x0 + x1) / 2, cy, (x1 - x0) / 2, (y1 - y0) / 2, rad);
        if (d > 0) { uint8_t g[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], (uint8_t)((focus ? 120 : 70) * expf(-d / (gl * 0.35f)))}; cv_blend(cv, x, y, g, 1);
                     if (focus && d > 2.5f) cv_blend(cv, x, y, C_WHITE, fminf(fminf(d - 2.5f, 5.0f - d), 1)); continue; }
        uint8_t c[4]; cv_mix(c, C_ACCENT, M_ACC_D, fminf(fmaxf((y + 0.5f - y0) / (y1 - y0), 0), 1)); cv_blend(cv, x, y, c, fminf(1, 0.5f - d));
    }
    for (int k = 0; k < 2; k++) { float sx = x1 + 0.03f * u + k * 0.045f * u, w = 0.022f * u;   // the stripes
        for (int y = (int)y0; y < (int)y1; y++) { float o = (y + 0.5f - cy) * sk; uint8_t c[4]; cv_mix(c, C_ACCENT, M_ACC_D, (y + 0.5f - y0) / (y1 - y0));
            for (int x = (int)(sx - o) - 1; x <= (int)(sx + w - o) + 1; x++) cv_blend(cv, x, y, c, fminf(fminf(x + 1 - (sx - o), sx + w - o - x), 1)); } }
    char w[32]; cv_caps(word, w, sizeof w); float px = 0.046f * u, tw = cv_sw(w, SF_BOLDI, px), ts = 0.026f * u, tx = (x0 + x1) / 2 - (tw + ts * 1.6f) / 2;
    static const uint8_t sh[4] = {0, 60, 20, 110};
    cv_tri(cv, tx + 1.5f, cy - ts + 1.5f, tx + 1.5f, cy + ts + 1.5f, tx + ts * 1.1f + 1.5f, cy + 1.5f, sh); cv_tri(cv, tx, cy - ts, tx, cy + ts, tx + ts * 1.1f, cy, C_WHITE);
    cv_sc(cv, tx + ts * 1.6f + 2, cy + 2, w, SF_BOLDI, px, 0, sh); cv_sc(cv, tx + ts * 1.6f, cy, w, SF_BOLDI, px, 0, C_WHITE);
}

// The pause menu (overlay_layout.h: the tabs, the cards, the slider's track): the tabs as a list in the panel, the open tab's rows as cards (a small caption
// over the value, the control on the right), Resume
static const char *const OVM_LABELS[OVM_N] = {"Show gamepad", "Opacity", "Edit layout", "Rumble", "Show FPS", "Mode", "Export save", "Import save", "Remap buttons", "Exit game", "Sharpen", "FPS limit", "Resume", ""};
static const char *const OVM_SHARPS[3] = {"Off", "Low", "High"};   // the Sharpen row (dm_menu_state bits 20-21 on the Picture tab)
static const char *const OVM_FPSCAPS[4] = {"30", "40", "45", "60"};   // the FPS limit row (bits 22-23 on the Picture tab; 30 = the game's own rate)
static const char *const OVM_MODES[2] = {"Handheld", "Docked"};
static const char *const OVM_MENU_KEYS[3][2] = {{"L  R", "Tabs"}, {"A", "Select"}, {"B", "Back"}};
static void ov_menu_panel(const ov_canvas *cv, int st) {
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H), p[4] = {0, 0, W, H};
    int tab = (st >> 8 & 1) | (st >> 30 & 1) << 1, sel = (st >> 4 & 15) - 1, op = st >> 9 & 127;   // (the tab's high bit at 30: front_android.c dm_menu_state)
    ov_m_panel(cv, OVM_MENU_KEYS);
    float lw = fmaxf(1.5f, 0.0026f * u);
    for (int t = 0; t < OVM_TABS; t++) {   // the tabs: an icon and the name; the open one white with an accent stripe on a lighter row
        float r[4]; ov_m_tab(W, H, t, r); float cy = r[1] + r[3] / 2; int on = t == tab;
        if (on) { cv_fill(cv, 0, r[1], r[2], r[1] + r[3], M_ROWHI); cv_fill(cv, 0, r[1], 0.0067f * u, r[1] + r[3], C_ACCENT); }
        if (on && sel == 0) cv_rring(cv, 0.012f * u, r[1] + 3, r[2] - 6, r[1] + r[3] - 3, 0.012f * u, 2, C_ACCENT);   // a controller on the tabs
        ov_m_icon(cv, t, 0.078f * u, cy, 0.042f * u, lw, on ? C_WHITE : M_TXT);
        cv_sc(cv, 0.142f * u, cy, OVM_TAB_NAMES[t], on ? SF_MED : SF_REG, 0.031f * u, 0, on ? C_WHITE : M_TXT);
    }
    float cpx = 0.0165f * u, vpx = 0.030f * u, dpx = 0.024f * u, pad = 0.026f * u, bpx = 0.024f * u;
    for (int row = 1; row < OVM_ROWS - 1; row++) {   // the tab's rows
        int it = ov_menu_item(tab, row); if (it < 0) continue;
        float r[4]; ov_menu_rowrect(p, row, r); float x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3], cy = (y0 + y1) / 2, cx1 = x1 - pad;
        ov_m_cardbg(cv, x0, y0, x1, y1, u, row == sel);
        char cap[32]; cv_caps(OVM_LABELS[it], cap, sizeof cap); cv_sc(cv, x0 + pad, y0 + 0.032f * u, cap, SF_REG, cpx, cpx * 0.10f, M_SUB);
        char val[80] = ""; int desc = 0; const uint8_t *vc = C_WHITE; float ctl = 0;   // the value under the caption; desc: a description (smaller); ctl: the control's width
        if (it == OVM_SHARPEN || it == OVM_FPSCAP) {
            const char *const *seg = it == OVM_SHARPEN ? OVM_SHARPS : OVM_FPSCAPS; int n = it == OVM_SHARPEN ? 3 : 4, cur = st >> (it == OVM_SHARPEN ? 20 : 22) & 3;
            float sw = 0; for (int k = 0; k < n; k++) sw = fmaxf(sw, cv_sw(seg[k], SF_MED, bpx)); sw += 0.036f * u; ctl = n * sw;
            ov_m_segments(cv, cx1 - ctl, cx1, cy, 0.052f * u, seg, n, cur, bpx);
            if (it == OVM_SHARPEN) snprintf(val, sizeof val, "%s", seg[cur]); else if (cur) snprintf(val, sizeof val, "%s fps", seg[cur]); else snprintf(val, sizeof val, "The game's own");
        } else if (it == OVM_MODE) {
            int cur = st >> 2 & 1; ctl = 2 * (cv_sw(OVM_MODES[0], SF_MED, bpx) + 0.036f * u);
            ov_m_segments(cv, cx1 - ctl, cx1, cy, 0.052f * u, OVM_MODES, 2, cur, bpx); snprintf(val, sizeof val, "%s", cur ? "Resolution" : "Performance");
        } else if (it == OVM_OPACITY) {   // the slider: its track (overlay_layout.h ov_menu_track, where a finger sets it), filled to the value, a knob
            float t[3]; ov_menu_track(p, row, t); float th = 0.008f * u, f = (float)(op - OV_OPACITY_MIN) / (OV_OPACITY_MAX - OV_OPACITY_MIN); f = f < 0 ? 0 : f > 1 ? 1 : f;
            cv_rrect(cv, t[0], t[1] - th / 2, t[0] + t[2], t[1] + th / 2, th / 2, M_FAINT); cv_rrect(cv, t[0], t[1] - th / 2, t[0] + f * t[2], t[1] + th / 2, th / 2, C_ACCENT);
            cv_disc(cv, t[0] + f * t[2], t[1], 0.016f * u, C_WHITE); snprintf(val, sizeof val, "%d%%", op); ctl = cx1 - t[0];
        } else if (it == OVM_EXPORT && (st >> 20 & 15) != OVX_IDLE) {   // the export's outcome in place of its button: its name, or why there is none
            char nm[64]; int x = dm_save_note(nm, sizeof nm); desc = 1;
            snprintf(val, sizeof val, "%s", x == OVX_BUSY ? "Exporting..." : x == OVX_DONE ? nm : x == OVX_NOSAVE ? "No save yet" : "Failed"); if (x == OVX_FAILED) vc = C_HOT;
        } else if (it == OVM_EDIT || it == OVM_EXPORT || it == OVM_IMPORT || it == OVM_PAD || it == OVM_EXIT) {   // a button
            static const char *const words[OVM_N] = {[OVM_EDIT] = "Edit", [OVM_EXPORT] = "Export", [OVM_IMPORT] = "Import", [OVM_PAD] = "Remap", [OVM_EXIT] = "Exit"};
            float bh = 0.052f * u; ctl = cv_sw(words[it], SF_MED, bpx) + 0.05f * u;
            ov_m_button(cv, cx1 - ctl, cy - bh / 2, cx1, cy + bh / 2, words[it], bpx, 0, 0); desc = 1;
            int ask = it == OVM_EXIT && st >> 29 & 1;   // the Exit game row once chosen (dm_menu_state bit 29), until it is chosen again or the menu closes
            snprintf(val, sizeof val, "%s", it == OVM_EDIT ? "Move and size the controls" : it == OVM_PAD ? "Per controller" : it == OVM_EXIT ? (ask ? "Press again to exit" : "Back to Android")
                                           : it == OVM_EXPORT ? "To the saves folder" : "From the saves folder");
            if (ask) { vc = C_ACCENT; desc = 0; }
        } else {   // a switch
            int on = st >> (it == OVM_OVERLAY ? 0 : it == OVM_RUMBLE ? 1 : 3) & 1; float sw = 0.085f * u, sh = 0.044f * u; ctl = sw;
            cv_rrect(cv, cx1 - sw, cy - sh / 2, cx1, cy + sh / 2, sh / 2, on ? C_ACCENT : M_FAINT);
            cv_disc(cv, on ? cx1 - sh / 2 : cx1 - sw + sh / 2, cy, sh / 2 - 0.006f * u, on ? C_WHITE : M_SUB); snprintf(val, sizeof val, "%s", on ? "On" : "Off");
        }
        float px = desc ? dpx : vpx; cv_sfit(val, SF_MED, px, cx1 - ctl - 0.02f * u - (x0 + pad));
        cv_sc(cv, x0 + pad, y0 + 0.070f * u, val, desc ? SF_REG : SF_MED, px, 0, desc && vc == C_WHITE ? M_TXT : vc);
    }
    ov_m_main(cv, "Resume", sel == OVM_ROWS - 1);
}
// The save picker and Remap buttons (overlay_layout.h OVS_*): the menu's frame, as front_android.c fills its rows (dm_save_rows): the title (and the page, or the
// controller) in the panel where the tabs are, the saves or buttons as cards (left: the name, right: its size or the button it takes), notes as lines, the
// import as a filled card, Back or Cancel as the main button
static const char *const OVS_KEYS[3][2] = {{"L  R", "Page"}, {"A", "Select"}, {"B", "Back"}};
static void ov_save_panel(const ov_canvas *cv, int st) {
    ov_srow rows[OVM_ROWS]; if (!dm_save_rows(rows)) return;
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H), p[4] = {0, 0, W, H}; int sel = (st >> 4 & 15) - 1;
    ov_m_panel(cv, OVS_KEYS);
    for (int row = 0; row < OVM_ROWS; row++) {
        ov_srow *q = &rows[row]; float r[4]; ov_menu_rowrect(p, row, r); float x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3], cy = (y0 + y1) / 2, pad = 0.026f * u;
        if (q->style == OVS_EMPTY) continue;
        if (q->style == OVS_TITLE) {   // in the panel, where the tabs are
            float tx = 0.055f * u, room = ov_m_edge(W, H, y0) - tx - 0.04f * u, tpx = 0.040f * u; char t[96]; cv_caps(q->left, t, sizeof t);
            while (tpx > 0.024f * u && cv_sw(t, SF_BOLDI, tpx) > room) tpx *= 0.94f;
            cv_sfit(t, SF_BOLDI, tpx, room); cv_sc(cv, tx, y0 + 0.05f * u, t, SF_BOLDI, tpx, 0, C_WHITE);
            if (q->right[0]) { cv_sfit(q->right, SF_REG, 0.025f * u, room); cv_sc(cv, tx, y0 + 0.11f * u, q->right, SF_REG, 0.025f * u, 0, M_SUB); }
            cv_fill(cv, tx, y0 + 0.15f * u, tx + 0.06f * u, y0 + 0.15f * u + fmaxf(2, 0.004f * u), C_ACCENT);
            continue;
        }
        if (q->style == OVS_BACK) { ov_m_main(cv, q->left, row == sel); continue; }
        if (q->style == OVS_NOTE) {   // a run of note rows (one text front_android.c cut into lines): together in one card, wrapped to its width
            if (row > 0 && rows[row - 1].style == OVS_NOTE) continue;
            char t[4 * 80] = ""; for (int k = row; k < OVM_ROWS && rows[k].style == OVS_NOTE; k++) { if (t[0]) strcat(t, " "); strcat(t, rows[k].left); }
            float pad = 0.026f * u, px = 0.024f * u, h = cv_swrap(NULL, x0 + pad, 0, r[2] - 2 * pad, t, SF_REG, px, M_TXT) + 2 * pad;
            ov_m_cardbg(cv, x0, y0, x1, y0 + h, u, 0); cv_swrap(cv, x0 + pad, y0 + pad, r[2] - 2 * pad, t, SF_REG, px, M_TXT);
            continue;
        }
        if (q->style == OVS_BUTTON) { float bh = 0.07f * u; ov_m_button(cv, x0, cy - bh / 2, x1, cy + bh / 2, q->left, 0.028f * u, 1, row == sel); continue; }
        ov_m_cardbg(cv, x0, y0, x1, y1, u, row == sel);   // an item
        float rw = q->right[0] ? cv_sw(q->right, SF_REG, 0.024f * u) : 0, room = r[2] - 2 * pad - (rw ? rw + 0.03f * u : 0);
        cv_sfit(q->left, SF_MED, 0.030f * u, room); cv_sc(cv, x0 + pad, cy, q->left, SF_MED, 0.030f * u, 0, C_WHITE);
        if (rw) cv_sc(cv, x1 - pad - rw, cy, q->right, SF_REG, 0.024f * u, 0, row == sel ? C_ACCENT : M_SUB);
    }
}
// the text entry (front_android.c front_text_input), in its own panel (overlay_layout.h ov_kb_rect): the game's title for it, the field with the text and a caret,
// the keyboard (ov_kb_*: the same rectangles the touches are tested against) with the highlighted key, and the controller's keys
static void ov_text_panel(const ov_canvas *cv) {
    char title[128], text[256]; int sel, shift; if (!dm_text_entry(title, sizeof title, text, sizeof text, &sel, &shift)) return;
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H), p[4]; ov_kb_rect(W, H, p);
    float x0 = p[0], y0 = p[1], pw = p[2], ph = p[3], pad = pw * 0.03f; uint8_t dim[4] = {0, 0, 0, 90};
    cv_fill(cv, 0, 0, W, H, dim);
    cv_rrect(cv, x0, y0, x0 + pw, y0 + ph, 0.014f * u, M_PANEL); uint8_t edge[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], 120}; cv_rring(cv, x0, y0, x0 + pw, y0 + ph, 0.014f * u, 1, edge);
    cv_fill(cv, x0 + 0.03f * u, y0 + ph * 0.04f, x0 + 0.03f * u + fmaxf(3, 0.0067f * u), y0 + ph * 0.10f, C_ACCENT);
    float tpx = ph * 0.045f; cv_sfit(title, SF_BOLDI, tpx, pw - 4 * pad); cv_sc(cv, x0 + 0.065f * u, y0 + ph * 0.07f, title, SF_BOLDI, tpx, 0, C_WHITE);
    float fy0 = y0 + ph * 0.14f, fy1 = y0 + ph * 0.27f; cv_rrect(cv, x0 + pad, fy0, x0 + pw - pad, fy1, 0.008f * u, M_CARD); cv_fill(cv, x0 + pad + 0.008f * u, fy1 - fmaxf(2, 0.004f * u), x0 + pw - pad - 0.008f * u, fy1, C_ACCENT);
    char shown[260]; snprintf(shown, sizeof shown, "%s|", text);   // the caret
    float xpx = ph * 0.06f; const char *s = shown; while (*s && cv_sw(s, SF_REG, xpx) > pw - 4 * pad) pf_next(&s);   // the end of a long text
    cv_sc(cv, x0 + 2 * pad, (fy0 + fy1) / 2, s, SF_REG, xpx, 0, C_WHITE);
    static const char *wide[4] = {"Shift", "Space", "Del", "OK"};
    for (int i = 0; i < OVK_N; i++) {
        float r[4]; ov_kb_key(i, p, r); int on = i == OVK_SHIFT && shift, hi = i == sel;
        cv_rrect(cv, r[0], r[1], r[0] + r[2], r[1] + r[3], r[3] * 0.16f, hi ? C_ACCENT : on ? M_CARD_HI : M_CARD);
        if (on && !hi) cv_rring(cv, r[0], r[1], r[0] + r[2], r[1] + r[3], r[3] * 0.16f, 1.5f, C_ACCENT);
        char one[2] = {(char)ov_kb_char(i, shift), 0}; const char *l = i < 40 ? one : wide[i - 40]; float kpx = r[3] * 0.42f;
        while (kpx > 6 && cv_sw(l, SF_MED, kpx) > r[2] * 0.85f) kpx *= 0.94f;   // a wide key's word fits inside it
        cv_sc(cv, r[0] + (r[2] - cv_sw(l, SF_MED, kpx)) / 2, r[1] + r[3] / 2, l, SF_MED, kpx, 0, hi ? M_DARK : i == OVK_OK ? C_ACCENT : C_WHITE);
    }
    const char *k = "A key   B delete   Y shift   + OK   - cancel"; float hpx = ph * 0.034f;   // one line, centred, smaller until it fits
    while (hpx > 6 && cv_sw(k, SF_REG, hpx) > pw - 2 * pad) hpx *= 0.94f;
    cv_sc(cv, x0 + (pw - cv_sw(k, SF_REG, hpx)) / 2, y0 + ph * 0.95f, k, SF_REG, hpx, 0, M_SUB);
}
// The layout editor's toolbar and prompt (overlay_layout.h ov_ed_*), where the touches find them
static const char *ov_ctl_name(const ov_ctl *c) {
    return c->kind == OV_STICK_L ? "Left stick" : c->kind == OV_STICK_R ? "Right stick" : c->kind == OV_DPAD ? "D-pad" : c->kind == OV_MENU ? "Menu button"
         : !strcmp(c->label, "-") ? "Minus" : !strcmp(c->label, "+") ? "Plus" : c->label;
}
static void ov_edit_panel(const ov_canvas *cv, int st) {
    float W = (float)cv->w, H = (float)cv->h, u = ov_m_unit(W, H), tb[4], pr[4]; ov_ed_rect(W, H, tb); ov_ed_prompt_rect(W, H, pr);
    int held = (st >> 4 & 15) - 1, sel = (st >> 8 & 255) - 1; uint8_t edge[4] = {C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], 120};
    static const char *const words[OVE_ALL] = {"-", "+", "Reset", "Cancel", "Save", "Snap to grid: Off", "Discard", "Keep editing"};
    cv_rrect(cv, tb[0], tb[1], tb[0] + tb[2], tb[1] + tb[3], 0.014f * u, M_PANEL); cv_rring(cv, tb[0], tb[1], tb[0] + tb[2], tb[1] + tb[3], 0.014f * u, 1, edge);
    char hint[96];
    if (sel >= 0 && sel < OV_N) snprintf(hint, sizeof hint, "%s: drag, pinch, - +", ov_ctl_name(&OV_CTLS[sel]));
    else snprintf(hint, sizeof hint, "Drag to move, pinch to resize");
    float hpx = tb[3] * 0.11f; cv_sfit(hint, SF_MED, hpx, tb[2] * 0.94f);
    cv_sc(cv, tb[0] + (tb[2] - cv_sw(hint, SF_MED, hpx)) / 2, tb[1] + tb[3] * 0.14f, hint, SF_MED, hpx, 0, sel >= 0 ? C_WHITE : M_TXT);
    for (int i = 0; i < OVE_N; i++) { float b[4]; ov_ed_button(i, tb, b); int grid = i == OVE_GRID && (st & OVE_GRID_ON);
        ov_m_button(cv, b[0], b[1], b[0] + b[2], b[1] + b[3], grid ? "Snap to grid: On" : words[i], b[3] * 0.40f, i == OVE_SAVE || grid, i == held); }
    if (!(st & 1)) return;
    uint8_t dim[4] = {0, 0, 0, 115}; cv_fill(cv, 0, 0, W, H, dim);   // the prompt over a darker frame
    cv_rrect(cv, pr[0], pr[1], pr[0] + pr[2], pr[1] + pr[3], 0.014f * u, M_PANEL); cv_rring(cv, pr[0], pr[1], pr[0] + pr[2], pr[1] + pr[3], 0.014f * u, 1, edge);
    char q[40] = "DISCARD YOUR CHANGES?"; float qpx = pr[3] * 0.13f; cv_sfit(q, SF_BOLDI, qpx, pr[2] * 0.9f);
    cv_sc(cv, pr[0] + (pr[2] - cv_sw(q, SF_BOLDI, qpx)) / 2, pr[1] + pr[3] * 0.27f, q, SF_BOLDI, qpx, 0, C_WHITE);
    for (int i = OVE_DISCARD; i <= OVE_KEEP; i++) { float b[4]; ov_ed_button(i, pr, b); ov_m_button(cv, b[0], b[1], b[0] + b[2], b[1] + b[3], words[i], b[3] * 0.40f, i == OVE_DISCARD, i == held); }
}
static void ov_shell(const ov_canvas *cv, int st) {   // the shell for menu state st (dm_menu_state), the canvas the window's size
    if (st >> 16 & 1) ov_text_panel(cv);   // a text entry is open
    else if (st & OVE_ST) ov_edit_panel(cv, st);
    else if (st & OVS_ST) ov_save_panel(cv, st);
    else ov_menu_panel(cv, st);
}
static void ov_panel(int st) {   // into the panel's rows of the upload buffer
    // drawn in ordinary (cached) memory, then copied into the upload buffer in one go: blending reads every texel back, and the mapped buffer is uncached
    // on the phones (drawn there directly, the shell took 1.5-1.8 s a change on the OnePlus 10 Pro)
    static uint8_t *buf; static size_t cap; size_t n = (size_t)g_ov.pw * g_ov.ph * 4;
    if (n > cap) { free(buf); buf = malloc(n); cap = buf ? n : 0; }
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    uint8_t *dst = g_ov.px + 4 * (size_t)g_ov.w * g_ov.py;   // (the panel's own row length: the window's width)
    if (!buf) { memset(dst, 0, n); return; }
    ov_canvas cv = {buf, g_ov.pw, 0, 0, g_ov.pw, g_ov.ph};
    memset(buf, 0, n); ov_shell(&cv, st); memcpy(dst, buf, n);
    clock_gettime(CLOCK_MONOTONIC, &b); double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    static int logged; if (logged < 40 || ms > 100) { logged++; VKLOG("[overlay] shell %#x drawn in %.1f ms (%ux%u)", st, ms, g_ov.pw, g_ov.ph); }
}
// The phone window uses the same raster as Vulkan, independently of the external swapchain: the window's size, drawn over it
JNIEXPORT jint JNICALL Java_org_nx2a_ExternalActivity_externalMenuState(JNIEnv *env, jclass cls) { (void)env; (void)cls; return dm_menu_state(); }
JNIEXPORT jintArray JNICALL Java_org_nx2a_ExternalActivity_externalMenuPixels(JNIEnv *env, jclass cls, jint W, jint H, jint st) {
    (void)cls;
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192 || st < 0 || (size_t)W * H > 16 * 1024 * 1024) return NULL;
    uint32_t w = (uint32_t)W, h = (uint32_t)H;
    uint8_t *pixels = calloc((size_t)w * h, 4); if (!pixels) return NULL;
    ov_canvas cv = {pixels, w, 0, 0, w, h}; ov_shell(&cv, st);
    size_t count = (size_t)w * h; jint *argb = malloc((count + 2) * sizeof *argb);
    if (!argb) { free(pixels); return NULL; }
    argb[0] = (jint)w; argb[1] = (jint)h;
    for (size_t i = 0; i < count; i++) { uint8_t *p = pixels + i * 4; argb[i + 2] = (jint)((uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]); }
    jintArray out = (*env)->NewIntArray(env, (jsize)(count + 2));
    if (out) (*env)->SetIntArrayRegion(env, out, 0, (jsize)(count + 2), argb);
    free(argb); free(pixels); return out;
}
static void ov_raster(uint8_t *px, uint32_t w, uint32_t h, uint32_t sh) {
    memset(px, 0, (size_t)w * (h + sh) * 4);
    int ts = h > 600 ? 3 : 2;
    for (int i = 0; i < OV_N && g_tt[g_ov.lay].ready != 1; i++) {   // (with a theme the controls are its sprites: the static layer stays empty)
        const ov_ctl *c = &OV_CTLS[i]; float b[4]; ov_box(c, (float)w, (float)h, b);
        if (c->kind == OV_MENU) continue;   // a sprite only
        if (c->kind == OV_DPAD) {
            for (int k = 0; k < 4; k++) { float ox, oy, dr; ov_dpad_dir(k, &ox, &oy, &dr); ov_circle(px, w, h, b[0] + ox * b[2], b[1] + oy * b[2], dr * b[2], 2.5f); }
            continue;
        }
        if (ov_round(c)) ov_circle(px, w, h, b[0], b[1], b[2], c->kind == OV_BUTTON ? 2.5f : 3.0f);
        else ov_box_ring(px, w, h, c, b, 2.5f);
        int ls = ts; while (ls > 1 && PF_CAP * ls > fminf(b[2], b[3])) ls--;   // smaller on a small button (-, +)
        if (c->label[0]) ov_text(px, w, h, b[0], b[1], c->label, ls);
    }
    ov_canvas strip = {px, w, 0, 0, w, h + sh};
    for (int i = 0; i < SP_N; i++) {   // the sprites (the knobs are drawn only as sprites, where the finger has them)
        float x = g_ov.sp[i][0], y = g_ov.sp[i][1], r = g_ov.sp[i][2], b[4] = {x, y, r, g_ov.sp[i][3]}; if (!r) continue;
        if (i == SP_DISC) cv_disc(&strip, x, y, r, C_WHITE);
        else if (i == SP_BLACK || i == SP_WHITE || i == SP_ACCENT) { static const uint8_t k[4] = {0, 0, 0, 255}; cv_rrect(&strip, x - r, y - r, x + r, y + r, 0, i == SP_BLACK ? k : i == SP_WHITE ? C_WHITE : C_ACCENT); }
        else if (OV_CTLS[i].kind == OV_MENU) ov_menu_icon(&strip, &OV_CTLS[i], b);
        else if (OV_CTLS[i].kind == OV_BUTTON) cv_box(&strip, &OV_CTLS[i], b, fminf(b[2], b[3]) * 0.07f, C_WHITE);   // inset as the disc's 0.93
        else ov_circle(px, w, h + sh, x, y, r, 2.5f);
    }
}
typedef struct { float pos[4], uv[4], alpha; } ov_quad;
static void ov_draw(VkCommandBuffer cb, const ov_quad *q) { vkCmdPushConstants(cb, g_ov.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *q, q); vkCmdDraw(cb, 6, 1, 0, 0); }
static void ov_rect(VkCommandBuffer cb, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha) {   // screen pixels, texels
    float W = (float)g_sext.width, H = (float)g_sext.height, tw = (float)g_ov.tw, th = (float)(g_ov.py + g_ov.ph);
    ov_quad q = {{x0 / W * 2 - 1, y0 / H * 2 - 1, x1 / W * 2 - 1, y1 / H * 2 - 1}, {u0 / tw, v0 / th, u1 / tw, v1 / th}, alpha}; ov_draw(cb, &q);
}
static void ov_sprite(VkCommandBuffer cb, int i, float sx, float sy, float r, float alpha) {   // sprite i centred on screen pixel (sx, sy), r screen pixels in half width
    float k = r / g_ov.sp[i][2], hx = g_ov.sp[i][2] + 3, hy = g_ov.sp[i][3] + 3;
    ov_rect(cb, sx - hx * k, sy - hy * k, sx + hx * k, sy + hy * k, g_ov.sp[i][0] - hx, g_ov.sp[i][1] - hy, g_ov.sp[i][0] + hx, g_ov.sp[i][1] + hy, alpha);
}
// The theme's sprites (overlay_layout.h ov_tsprite): the design's CSS pixels scaled by the window's height over the design's (and dm_overlay_scale), each at its
// offset from its control's centre. Decorations first, then each control in the table's order: a held button, d-pad direction or stick in its held sprite,
// a stick's knob where the finger has it (as far as the design lets it travel); the menu button last (ov_theme_menu), over everything
static void ov_tsp(VkCommandBuffer cb, const ov_theme *t, const ov_tsprite *s, float cx, float cy, float k, float alpha) {
    float W = (float)g_sext.width, H = (float)g_sext.height, x0 = cx + s->ox * k, y0 = cy + s->oy * k, x1 = x0 + s->w / t->scale * k, y1 = y0 + s->h / t->scale * k;
    ov_quad q = {{x0 / W * 2 - 1, y0 / H * 2 - 1, x1 / W * 2 - 1, y1 / H * 2 - 1}, {s->u / t->w, s->v / t->h, (s->u + s->w) / t->w, (s->v + s->h) / t->h}, alpha}; ov_draw(cb, &q);
}
static float ov_glow(int64_t t, int64_t now) {   // a held button's glow: bright at the press, fading to a steady one
    if (!t) return 0;
    float f = (float)(now - t) / OV_FLASH_NS; return f >= 1 ? 0.22f : 0.22f + 0.48f * (1 - f) * (1 - f);
}
// The frame rate counter: the game's frames shown per second, measured over half a second, in the window's top left corner on a dark block
static float ov_fps_cell(void) {   // a character cell's width in texels: the widest advance of OV_FPS_CHARS (8 columns: A, O, T), a clear texel each side
    static int adv;
    if (!adv) for (const char *c = OV_FPS_CHARS; *c; c++) { char one[2] = {*c, 0}; int a = pf_width(dm_overlay_font(), one); if (a > adv) adv = a; }
    return (float)adv * g_ov.gs + 2;
}
static void ov_fps_glyphs(void) {   // the characters into the rows under the panel
    ov_canvas cv = {g_ov.px, g_ov.w, 0, g_ov.h + g_ov.sh, g_ov.w, g_ov.gh};
    int n = (int)strlen(OV_FPS_CHARS);   // white, then the same characters in red (a temperature too hot: ov_fps) after them
    for (int i = 0; i < 2 * n; i++) { char one[2] = {OV_FPS_CHARS[i % n], 0}; cv_text(&cv, i * ov_fps_cell() + 1 + g_ov.gs * 0.5f, g_ov.gh / 2.0f, one, (int)g_ov.gs, i < n ? C_WHITE : C_HOT); }
}
__attribute__((weak)) void dm_android_temps(int *soc, int *bat) { (void)soc; (void)bat; }   // (front_android.c; absent in other builds)
static void ov_fps(VkCommandBuffer cb, int64_t now) {   // one value a row: FPS, then the chip's and the battery's temperature where the device gives them
    static uint64_t n0; static int64_t t0; static int fps = -1;
    uint64_t n = atomic_load(&g_fps_frames);
    if (!t0 || now - t0 > 2000000000ll) { t0 = now; n0 = n; fps = -1; }   // the first frame, or after a pause in the presents (the menu, the background): measure anew
    else if (now - t0 >= 500000000ll) { fps = (int)((double)(n - n0) * 1e9 / (double)(now - t0) + 0.5); t0 = now; n0 = n; }
    char rows[3][16]; int nr = 0, hot[3] = {0};
    if (fps < 0) snprintf(rows[nr++], 16, "FPS --"); else snprintf(rows[nr++], 16, "FPS %d", fps > 999 ? 999 : fps);
    int soc = -1000, bat = -1000; dm_android_temps(&soc, &bat);   // front_android.c: the chip's hottest CPU / GPU zone and the battery, whole degrees C (-1000: not known)
    // red from where it is dangerously hot: the chip at 80 C (phones throttle hard in the high 80s), the battery at 45 C (Android's warnings and charging limits)
    if (soc > -100 && soc < 1000) { hot[nr] = soc >= 80; snprintf(rows[nr++], 16, "SOC %dC", soc); }
    if (bat > -100 && bat < 1000) { hot[nr] = bat >= 45; snprintf(rows[nr++], 16, "BAT %dC", bat); }
    float k = (float)g_sext.width / g_ov.w, cw = ov_fps_cell() * k, ch = (float)g_ov.gh * k, m = (float)g_sext.height * 0.02f, pad = ch * 0.2f, lh = ch * 1.1f;
    float bx = g_ov.sp[SP_BLACK][0], by = g_ov.sp[SP_BLACK][1], gy = (float)(g_ov.h + g_ov.sh); size_t wmax = 0;
    for (int r = 0; r < nr; r++) if (strlen(rows[r]) > wmax) wmax = strlen(rows[r]);
    float x1 = m + ch * 0.5f + wmax * cw, y1 = m + 2 * pad + ch + (nr - 1) * lh, ax = g_ov.sp[SP_ACCENT][0], ay = g_ov.sp[SP_ACCENT][1];
    ov_rect(cb, m, m, x1, y1, bx - 2, by - 2, bx + 2, by + 2, 0.72f);   // an anthracite box, an accent stripe under it
    ov_rect(cb, m, y1, x1, y1 + fmaxf(2, roundf(ch * 0.12f)), ax - 2, ay - 2, ax + 2, ay + 2, 1);
    for (int r = 0; r < nr; r++) {
        float x = m + ch * 0.25f, y = m + pad + r * lh;
        for (int i = 0; rows[r][i]; i++, x += cw) {
            const char *c = strchr(OV_FPS_CHARS, rows[r][i]); if (!c || rows[r][i] == ' ') continue;
            float u = (float)(c - OV_FPS_CHARS + (hot[r] ? (int)strlen(OV_FPS_CHARS) : 0)) * ov_fps_cell(); ov_rect(cb, x, y, x + cw, y + ch, u, gy, u + ov_fps_cell(), gy + g_ov.gh, 1);
        }
    }
}
static void ov_theme_draw(VkCommandBuffer cb, const ov_theme *t, float alpha) {   // (alpha: the player's opacity; each control at its own size factor)
    float W = (float)g_sext.width, H = (float)g_sext.height, k = H / t->dh * dm_overlay_scale(), b[4]; const ov_ctl *L = OV_LAYOUTS[t->layout];
    for (int j = 0; j < t->n; j++) if (t->sp[j].state == OVT_DECOR) { const ov_ctl *c = &L[t->sp[j].ctl]; ov_box(c, W, H, b); ov_tsp(cb, t, &t->sp[j], b[0], b[1], k * ov_cscale(c), alpha); }
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &L[i]; if (c->kind == OV_MENU) continue;
        ov_box(c, W, H, b); float kc = k * ov_cscale(c); int stick = c->kind == OV_STICK_L || c->kind == OV_STICK_R, held = c->kind != OV_DPAD && dm_overlay_pressed(i, 0) != 0;
        float kx = 0, ky = 0; if (stick) { dm_overlay_knob(c->kind == OV_STICK_R, &kx, &ky); kx *= t->travel * kc; ky *= t->travel * kc; }
        for (int j = 0; j < t->n; j++) {
            const ov_tsprite *s = &t->sp[j]; if (s->ctl != i || s->state == OVT_DECOR) continue;
            if (s->state == OVT_KNOB) { ov_tsp(cb, t, s, b[0] + kx, b[1] + ky, kc, alpha); continue; }
            int on = c->kind == OV_DPAD ? s->dir >= 0 && dm_overlay_pressed(i, OV_DPAD_BITS[s->dir]) != 0 : held;
            if (s->state == (on ? OVT_HELD : OVT_IDLE)) ov_tsp(cb, t, s, b[0], b[1], kc, alpha);
        }
    }
}
static void ov_theme_menu(VkCommandBuffer cb, const ov_theme *t, int i, float alpha) {
    const ov_ctl *c = &OV_LAYOUTS[t->layout][i]; float W = (float)g_sext.width, H = (float)g_sext.height, b[4]; ov_box(c, W, H, b);
    int held = dm_overlay_pressed(i, 0) != 0;
    for (int j = 0; j < t->n; j++) if (t->sp[j].ctl == i && t->sp[j].state == (held ? OVT_HELD : OVT_IDLE)) ov_tsp(cb, t, &t->sp[j], b[0], b[1], H / t->dh * dm_overlay_scale() * ov_cscale(c), alpha);
}
static void ov_block(VkCommandBuffer cb, int sp, float x0, float y0, float x1, float y1, float alpha) {   // a solid rectangle in a block sprite's colour
    float bx = g_ov.sp[sp][0], by = g_ov.sp[sp][1]; ov_rect(cb, x0, y0, x1, y1, bx - 2, by - 2, bx + 2, by + 2, alpha);
}
static void ov_frame(VkCommandBuffer cb, int sp, const float b[4], float t, float alpha) {   // a rectangle's outline around a control's box (b: ov_box), t pixels thick
    float x0 = b[0] - b[2] - t, x1 = b[0] + b[2] + t, y0 = b[1] - b[3] - t, y1 = b[1] + b[3] + t;
    ov_block(cb, sp, x0, y0, x1, y0 + t, alpha); ov_block(cb, sp, x0, y1 - t, x1, y1, alpha); ov_block(cb, sp, x0, y0 + t, x0 + t, y1 - t, alpha); ov_block(cb, sp, x1 - t, y0 + t, x1, y1 - t, alpha);
}
static void ov_render(VkCommandBuffer cb, int mode, int menu, int fps) {
    float W = (float)g_sext.width, H = (float)g_sext.height, op = dm_overlay_opacity();
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    int edit = menu >= 0 && !(menu >> 16 & 1) && (menu & OVE_ST);   // the layout editor: the gamepad over the held frame, every control (shown or not)
    if (edit) mode = 1;
    float bx = g_ov.sp[SP_BLACK][0], by = g_ov.sp[SP_BLACK][1];
    if (menu >= 0 && !edit) {   // paused: the frame dimmed, the shell 1:1 over the window, and the menu button (which closes it)
        ov_rect(cb, 0, 0, W, H, bx - 2, by - 2, bx + 2, by + 2, 0.45f);
        ov_rect(cb, 0, 0, W, H, 0, (float)g_ov.py, (float)g_ov.pw, (float)(g_ov.py + g_ov.ph), 1);
    } else if (edit) {
        ov_rect(cb, 0, 0, W, H, bx - 2, by - 2, bx + 2, by + 2, 0.30f);
        if (menu & OVE_GRID_ON) {
            float step = H / OVE_GRID_ROWS, t = fmaxf(1, H * 0.001f);
            for (int i = 1; i * step < W; i++) { float x = i * step; ov_block(cb, SP_WHITE, x - t / 2, 0, x + t / 2, H, 0.20f); }
            for (int i = 1; i < OVE_GRID_ROWS; i++) { float y = i * step; ov_block(cb, SP_WHITE, 0, y - t / 2, W, y + t / 2, 0.20f); }
        }
    }
    const ov_theme *th = g_tt[g_ov.lay].ready == 1 ? g_tt[g_ov.lay].t : NULL; int tbound = 0;   // (the theme's texture bound)
    if ((menu < 0 || edit) && mode == 1 && th) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_tt[g_ov.lay].set, 0, NULL); tbound = 1;
        ov_theme_draw(cb, th, op);
    } else if ((menu < 0 || edit) && mode == 1) {   // without a theme: the outlines (their opacity: 60% looks as they always did), the knobs and the glows
        float fo = fminf(1, op / 0.6f);
        ov_rect(cb, 0, 0, W, H, 0, 0, (float)g_ov.w, (float)g_ov.h, fo);
        for (int i = 0; i < OV_N; i++) { const ov_ctl *c = &OV_CTLS[i]; float b[4]; ov_box(c, W, H, b);
            if (c->kind == OV_STICK_L || c->kind == OV_STICK_R) {
                float dx, dy; dm_overlay_knob(c->kind == OV_STICK_R, &dx, &dy); float travel = b[2] * (1 - OV_KNOB);   // the knob's edge reaches the ring
                ov_sprite(cb, i, b[0] + dx * travel, b[1] + dy * travel, b[2] * OV_KNOB, fo);
            } else if (c->kind == OV_BUTTON) { float g = ov_glow(dm_overlay_pressed(i, 0), now); if (g <= 0) continue;
                if (!ov_round(c)) ov_sprite(cb, i, b[0], b[1], b[2], g); else ov_sprite(cb, SP_DISC, b[0], b[1], b[2] * 0.93f, g); }
            else if (c->kind == OV_DPAD) for (int k = 0; k < 4; k++) {
                float g = ov_glow(dm_overlay_pressed(i, OV_DPAD_BITS[k]), now), ox, oy, dr; if (g <= 0) continue;
                ov_dpad_dir(k, &ox, &oy, &dr); ov_sprite(cb, SP_DISC, b[0] + ox * b[2], b[1] + oy * b[2], dr * b[2] * 0.93f, g);
            }
        }
    }
    for (int i = 0; i < OV_N; i++) if (OV_CTLS[i].kind == OV_MENU && mode) {   // the menu button, on top of the controls, where the player put it
        float ma = mode == 1 && (menu < 0 || edit) ? fmaxf(op, 0.5f) : OV_MENU_ALPHA;   // (findable at any opacity)
        if (th) { if (!tbound) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_tt[g_ov.lay].set, 0, NULL); tbound = 1; }
                  ov_theme_menu(cb, th, i, ma); continue; }
        const ov_ctl *c = &OV_CTLS[i]; float b[4], g = ov_glow(dm_overlay_pressed(i, 0), now); ov_box(c, W, H, b);
        ov_sprite(cb, i, b[0], b[1], b[2], ma);
        if (g > 0) ov_sprite(cb, i, b[0], b[1], b[2], g);
    }
    if (tbound) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_ov.set, 0, NULL);   // the rest is the procedural texture's
    if (edit) {   // every control's box faintly, the chosen one in the accent colour; the toolbar and the prompt (with its darker frame) from the shell's raster
        int sel = (menu >> 8 & 255) - 1; float t = fmaxf(2, H * 0.003f);
        for (int i = 0; i < OV_N; i++) { float b[4]; ov_box(&OV_CTLS[i], W, H, b); if (i == sel) ov_frame(cb, SP_ACCENT, b, 2 * t, 1); else ov_frame(cb, SP_WHITE, b, t, 0.35f); }
        ov_rect(cb, 0, 0, W, H, 0, (float)g_ov.py, (float)g_ov.pw, (float)(g_ov.py + g_ov.ph), 1);
    }
    if (fps && menu < 0) ov_fps(cb, now);
}
// the theme's atlas for a layout into its texture, once (overlay_theme.c reads it at the first present that shows the gamepad in that layout): copied in this
// present's command buffer; its staging buffer goes at a later present, once the copy has run (its slot's fence signalled, or the slot came round again)
static void ov_theme_upload(VkCommandBuffer cb, uint32_t k, int lay) {
    typeof(g_tt[0]) *T = &g_tt[lay];
    if (T->up && (k == T->upk || vkWaitForFences(V.dev, 1, &g_pfence[T->upk], 1, 0) == VK_SUCCESS)) { vkDestroyBuffer(V.dev, T->up, NULL); vkFreeMemory(V.dev, T->upm, NULL); T->up = VK_NULL_HANDLE; }
    if (T->ready) return;
    const ov_theme *t = dm_overlay_theme(lay); T->ready = -1;
    if (!t || !t->px) return;
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {(uint32_t)t->w, (uint32_t)t->h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHK(vkCreateImage(V.dev, &ic, NULL, &T->img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, T->img, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &T->mem)); VKCHK(vkBindImageMemory(V.dev, T->img, T->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, T->img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHK(vkCreateImageView(V.dev, &vi, NULL, &T->view));
    VkDeviceSize n = (VkDeviceSize)t->w * t->h * 4;
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bc, NULL, &T->up)); VkMemoryRequirements bq; vkGetBufferMemoryRequirements(V.dev, T->up, &bq);
    VkMemoryAllocateInfo bi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bq.size, vk_mem_type(bq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &bi, NULL, &T->upm)); VKCHK(vkBindBufferMemory(V.dev, T->up, T->upm, 0));
    void *px; VKCHK(vkMapMemory(V.dev, T->upm, 0, VK_WHOLE_SIZE, 0, &px)); memcpy(px, t->px, (size_t)n);   // (freeing the memory unmaps it)
    dm_overlay_theme_done(t);
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_ov.pool, 1, &g_ov.dsl};
    if (vkAllocateDescriptorSets(V.dev, &da, &T->set)) { VKRED_ONCE(95, "overlay theme descriptor set failed: the gamepad is drawn without its theme"); return; }
    VkDescriptorImageInfo ii = {g_ov.samp, T->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, T->set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
    vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, T->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = {0, (uint32_t)t->w, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)t->w, (uint32_t)t->h, 1}};
    vkCmdCopyBufferToImage(cb, T->up, T->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    T->t = t; T->upk = k; T->ready = 1;
}
static int overlay_ready(VkCommandBuffer cb, uint32_t k, int menu) {   // k: this present's slot (its fence is not signalled yet)
    uint32_t w = g_sext.width / 2, h = g_sext.height / 2; int lay = dm_overlay_layout();
    if (!g_ov.dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b}; VKCHK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_ov.dsl));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ov_quad)};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_ov.dsl, 1, &pr}; VKCHK(vkCreatePipelineLayout(V.dev, &pl, NULL, &g_ov.layout));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 3, 1, &ps};   // the procedural texture, a theme atlas per layout
        VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &g_ov.pool));
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_ov.pool, 1, &g_ov.dsl}; VKCHK(vkAllocateDescriptorSets(V.dev, &ai, &g_ov.set));
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VKCHK(vkCreateSampler(V.dev, &si, NULL, &g_ov.samp));
    }
    if (g_ov.pfmt != g_sfmt) {   // the pipeline renders into the swapchain's format (the last format's goes: swap_destroy waited for the device on every path that changes g_sfmt)
        if (g_ov.pipe) { vkDestroyPipeline(V.dev, g_ov.pipe, NULL); g_ov.pipe = VK_NULL_HANDLE; }
        VkShaderModule vm, fm;
        VkShaderModuleCreateInfo vc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof overlay_vert_spv, overlay_vert_spv}; VKCHK(vkCreateShaderModule(V.dev, &vc, NULL, &vm));
        VkShaderModuleCreateInfo fc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof overlay_frag_spv, overlay_frag_spv}; VKCHK(vkCreateShaderModule(V.dev, &fc, NULL, &fm));
        VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL},
                                                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
        VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xf};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
        VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &g_sfmt};
        // (the pass follows the swapchain's format by itself: ov_rp makes it again when g_sfmt changed, and swap_destroy dropped the framebuffers then. Dropping it here
        // as well destroyed the pass and framebuffer the sharpening pass had just begun in this command buffer: a crash in vkEndCommandBuffer, DM_VK_SHARPEN on the OP)
        VkRenderPass orp = V.has_dynrender ? VK_NULL_HANDLE : ov_rp();
        if (!V.has_dynrender && !orp) { vkDestroyShaderModule(V.dev, vm, NULL); vkDestroyShaderModule(V.dev, fm, NULL); return 0; }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, orp ? NULL : &rci, 0, 2, ss, &vi, &ia, NULL, &vs, &rs, &ms, NULL, &cbs, &dy, g_ov.layout, orp};
        VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &g_ov.pipe);
        vk_pipeline_cache_changed();
        vkDestroyShaderModule(V.dev, vm, NULL); vkDestroyShaderModule(V.dev, fm, NULL);
        if (r) { VKRED_ONCE(92, "overlay pipeline failed: %d", r); return 0; }
        g_ov.pfmt = g_sfmt;
    }
    ov_theme_upload(cb, k, lay);
    if (g_ov.w != w || g_ov.h != h || g_ov.lay != lay || (g_tt[lay].ready != 1 && g_ov.cver != dm_overlay_custom_ver())) {   // (re)rasterize for this window size and layout
        g_ov.cver = dm_overlay_custom_ver();   // (and, without a theme, the player's own layout: the outlines are in the static layer)
        vkDeviceWaitIdle(V.dev);
        if (g_ov.view) { vkDestroyImageView(V.dev, g_ov.view, NULL); vkDestroyImage(V.dev, g_ov.img, NULL); vkFreeMemory(V.dev, g_ov.mem, NULL); vkDestroyBuffer(V.dev, g_ov.up, NULL); vkFreeMemory(V.dev, g_ov.upm, NULL); }
        // the texture: the static layer and the counter's characters (w wide, half the window's), then the shell's raster (the window's size, 1:1: its letters are
        // smooth); in the buffer each region packed at its own width
        g_ov.lay = lay; uint32_t sh = ov_layout_sprites(w, h);
        g_ov.gs = (uint32_t)fmaxf(2, roundf(h * 0.022f / PF_CAP)); g_ov.gh = PF_CAP * g_ov.gs + 4;   // the counter's characters: 2.2% of the window's height
        g_ov.pw = g_sext.width; g_ov.ph = g_sext.height; g_ov.py = h + sh + g_ov.gh; g_ov.tw = g_ov.pw > w ? g_ov.pw : w;
        uint32_t th = g_ov.py + g_ov.ph;
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {g_ov.tw, th, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VKCHK(vkCreateImage(V.dev, &ic, NULL, &g_ov.img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, g_ov.img, &rq);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &g_ov.mem)); VKCHK(vkBindImageMemory(V.dev, g_ov.img, g_ov.mem, 0));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_ov.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHK(vkCreateImageView(V.dev, &vi, NULL, &g_ov.view));
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, ((VkDeviceSize)w * g_ov.py + (VkDeviceSize)g_ov.pw * g_ov.ph) * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bc, NULL, &g_ov.up)); VkMemoryRequirements bq; vkGetBufferMemoryRequirements(V.dev, g_ov.up, &bq);
        VkMemoryAllocateInfo bi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bq.size, vk_mem_type(bq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &bi, NULL, &g_ov.upm)); VKCHK(vkBindBufferMemory(V.dev, g_ov.up, g_ov.upm, 0));
        void *px; VKCHK(vkMapMemory(V.dev, g_ov.upm, 0, VK_WHOLE_SIZE, 0, &px)); g_ov.px = px;
        g_ov.w = w; g_ov.h = h; g_ov.sh = sh; ov_raster(g_ov.px, w, h, sh); memset(g_ov.px + (size_t)w * (h + sh) * 4, 0, (size_t)w * g_ov.gh * 4 + (size_t)g_ov.pw * g_ov.ph * 4); ov_fps_glyphs();
        VkDescriptorImageInfo ii = {g_ov.samp, g_ov.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_ov.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
        vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
        g_ov.uploaded = 0; g_ov.panel = -1;
    }
    int only_panel = 0;   // the regions to upload: all, or only the shell's
    if (g_ov.uploaded && menu >= 0 && menu != g_ov.panel) {   // only the panel, after the last copy out of the buffer has run
        VkFence f[8]; uint32_t n = 0; for (uint32_t i = 0; i < 8; i++) if (i != k) f[n++] = g_pfence[i];
        vkWaitForFences(V.dev, n, f, 1, 1000000000ull);
        only_panel = 1; g_ov.uploaded = 0;
    }
    if (menu >= 0 && menu != g_ov.panel) { ov_panel(menu); g_ov.panel = menu; }
    if (!g_ov.uploaded) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, only_panel ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_ov.img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy c[2] = {{(VkDeviceSize)g_ov.w * g_ov.py * 4, g_ov.pw, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, (int32_t)g_ov.py, 0}, {g_ov.pw, g_ov.ph, 1}},
                                  {0, g_ov.w, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {g_ov.w, g_ov.py, 1}}};
        vkCmdCopyBufferToImage(cb, g_ov.up, g_ov.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, only_panel ? 1 : 2, c);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        g_ov.uploaded = 1;
    }
    return 1;
}
// ---- frame pacing on the display (VK_GOOGLE_display_timing) ----
// Without a time of its own a frame goes on screen at the first vsync after it is latched. Mario vs. Donkey Kong at 60 fps on the Legion Y700's 120 Hz panel was ready about
// 4.6 ms before that latch (SurfaceFlinger's latency: 12.9 ms from ready to shown, GPU 9.5-10.5 ms a frame), and about one frame in ten missed it by a millisecond or so: shown
// for three vsyncs, the next one for one (11 of 125 frames each way, a visible stutter; Hades II's title, ready far earlier, showed every frame for two). Each frame now gets a
// desired present time: the last one's plus the game's period, on the display's vsync grid (anchored at a past frame's actual present time), half a vsync before that vsync so
// it cannot go out early. A frame that would not be ready in time moves the cadence to the first vsync it can make (a "resync": one longer frame, and from then on a vsync of
// slack). The pause menu's redraws carry no time. DM_VK_PACE=0 (live) presents without times
static uint64_t g_refresh_ns, g_pace_target, g_pace_ideal, g_pace_anchor, g_pace_n, g_pace_resync, g_pace_slip; static uint32_t g_pace_id; static int g_pace_on = 1;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void pace_reset(void) { g_pace_target = g_pace_ideal = g_pace_anchor = g_refresh_ns = 0; }
static uint64_t pace_snap(uint64_t t) {   // the first vsync at or after t on the grid through the anchor
    uint64_t r = g_refresh_ns, a = g_pace_anchor;
    if (t <= a) return a - (a - t) / r * r;
    return a + (t - a + r - 1) / r * r;
}
static uint64_t pace_desired(double period) {   // the frame's desired present time (0 = none)
    if (!V.has_dtiming || !g_swap) return 0;
    if (g_pace_n % 30 == 0) { const char *e = getenv("DM_VK_PACE"); g_pace_on = !(e && !atoi(e)); }
    if (!g_refresh_ns || g_pace_n % 120 == 0) {
        VkRefreshCycleDurationGOOGLE rc; uint64_t was = g_refresh_ns;
        if (vkGetRefreshCycleDurationGOOGLE(V.dev, g_swap, &rc) == VK_SUCCESS && rc.refreshDuration > 1000000) g_refresh_ns = rc.refreshDuration;
        if (g_refresh_ns && g_refresh_ns != was) { VKLOG("[perf] display timing: refresh %.3f ms", g_refresh_ns / 1e6); g_pace_target = 0; }
    }
    VkPastPresentationTimingGOOGLE pt[16]; uint32_t np = 16;
    if (vkGetPastPresentationTimingGOOGLE(V.dev, g_swap, &np, pt) >= 0)
        for (uint32_t i = 0; i < np; i++) {
            present_time_record(pt[i].presentID, pt[i].actualPresentTime);
            if (pt[i].actualPresentTime > g_pace_anchor) g_pace_anchor = pt[i].actualPresentTime;
            if (pt[i].desiredPresentTime && pt[i].actualPresentTime > pt[i].desiredPresentTime + g_refresh_ns) g_pace_slip++;   // went out a vsync or more after its own
        }
    g_pace_n++;
    if (g_pace_n % 600 == 0) VKLOG("[perf] display timing: %llu frames, %llu resyncs, %llu shown a vsync or more after their time (cumulative)%s",
                                   (unsigned long long)g_pace_n, (unsigned long long)g_pace_resync, (unsigned long long)g_pace_slip, g_pace_on ? "" : "; off (DM_VK_PACE=0)");
    if (!g_pace_on || !g_refresh_ns || !g_pace_anchor || period <= 0) { g_pace_target = 0; return 0; }
    // The earliest vsync this frame can make: its GPU work (submitted just before), the blit, then the latch a vsync before it is shown. When the cadence's next vsync is
    // earlier, the cadence restarts half a vsync further out than that, so the next frames have room for the jitter: restarting at the earliest itself made 31 long frames
    // in 4,200 (5 shown late), and going on until the target was under a vsync away made 54 in 3,600 go out late
    double vk_gpu_ms_recent(void); double g = vk_gpu_ms_recent(); uint64_t gpu = (uint64_t)((g > 4 ? g : 4) * 1e6) + 3000000;
    // The cadence advances unsnapped (g_pace_ideal) and only the frame's vsync is snapped: snapping each target and adding the period to it lost the fraction,
    // so a period that is not a whole number of vsyncs always rounded up (40 fps on a 60 Hz panel: 25 ms came out as 2 vsyncs every frame, 30 fps, on a Retroid Pocket 5)
    uint64_t now = mono_ns(), t = 0;
    if (g_pace_target) { g_pace_ideal += (uint64_t)(period * 1e9); t = pace_snap(g_pace_ideal - g_refresh_ns / 2); }
    if (t < now + gpu + g_refresh_ns) { if (g_pace_target) g_pace_resync++; t = pace_snap(now + gpu + g_refresh_ns * 3 / 2); g_pace_ideal = t; }
    g_pace_target = t;
    return t - g_refresh_ns / 2;
}
// ---- Sharpening at present (DM_VK_SHARPEN=<0..100>, also the pause menu's Sharpen row, which sets it; 0 = off, the blit as before). The game's window image (Batman: Arkham
// Knight's 1920x1080, already scaled up by the game from its 960x540 scene) is drawn into the picture's rectangle by a contrast-adaptive sharpening shader (shaders/sharpen.*)
// in place of vkCmdBlitImage's linear filter; the overlay and the menu are drawn after it, unsharpened. Read again every 30 presents ----
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkSampler samp; VkPipeline pipe, upipe; VkFormat pfmt, upfmt; VkImage img[4]; VkImageView view[4]; VkDescriptorSet set[4]; int next; } g_sh;
static int sharpen_level(void) { static int v, n; if (n++ % 30 == 0) { const char *e = getenv("DM_VK_SHARPEN"); v = e ? atoi(e) : 0; v = v < 0 ? 0 : v > 100 ? 100 : v; } return v; }
// DM_VK_UPSCALE=gsr (live, read again every 30 presents): the picture is drawn by Snapdragon GSR 1 (shaders/upscale.frag: edge-directed, sharpening built in) in place of the
// linear blit or the sharpening; DM_VK_UPSCALE_SHARP=<sharpness>, GSR's EdgeSharpness (2.0 by default, upstream's). Unset or 0: as before
static float upscale_sharp(void) {
    static float v; static int n; if (n++ % 30 == 0) { const char *e = getenv("DM_VK_UPSCALE"), *k = getenv("DM_VK_UPSCALE_SHARP");
        int on = e && (!strcmp(e, "gsr") || atoi(e) > 0); v = k ? (float)atof(k) : 2.0f; v = !on ? 0 : v < 0.001f ? 0.001f : v > 4 ? 4 : v; }   // (on: above 0)
    return v;
}
static int sharpen_ready(int up) {
    if (!g_sh.dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b}; VKCHK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_sh.dsl));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_sh.dsl, 1, &pr}; VKCHK(vkCreatePipelineLayout(V.dev, &pl, NULL, &g_sh.layout));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4, 1, &ps};
        VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &g_sh.pool));
        for (int i = 0; i < 4; i++) { VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_sh.pool, 1, &g_sh.dsl}; VKCHK(vkAllocateDescriptorSets(V.dev, &ai, &g_sh.set[i])); }
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VKCHK(vkCreateSampler(V.dev, &si, NULL, &g_sh.samp));
    }
    VkPipeline *pp = up ? &g_sh.upipe : &g_sh.pipe; VkFormat *pf = up ? &g_sh.upfmt : &g_sh.pfmt;
    if (*pf == g_sfmt && *pp) return 1;
    if (*pp) { vkDestroyPipeline(V.dev, *pp, NULL); *pp = VK_NULL_HANDLE; }
    VkRenderPass orp = V.has_dynrender ? VK_NULL_HANDLE : ov_rp(); if (!V.has_dynrender && !orp) return 0;
    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo vc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof sharpen_vert_spv, sharpen_vert_spv}; VKCHK(vkCreateShaderModule(V.dev, &vc, NULL, &vm));
    VkShaderModuleCreateInfo fc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, up ? sizeof upscale_frag_spv : sizeof sharpen_frag_spv, up ? upscale_frag_spv : sharpen_frag_spv};
    VKCHK(vkCreateShaderModule(V.dev, &fc, NULL, &fm));
    VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL},
                                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &g_sfmt};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, orp ? NULL : &rci, 0, 2, ss, &vi, &ia, NULL, &vs, &rs, &ms, NULL, &cbs, &dy, g_sh.layout, orp};
    VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, pp);
    vkDestroyShaderModule(V.dev, vm, NULL); vkDestroyShaderModule(V.dev, fm, NULL);
    if (r) { *pp = VK_NULL_HANDLE; VKRED_ONCE(115, "%s pipeline failed: %d (the picture is blitted as before)", up ? "upscale" : "sharpening", r); return 0; }
    *pf = g_sfmt;
    if (up) VKLOG("present: Snapdragon GSR 1 upscale on (DM_VK_UPSCALE, sharpness %.2f)", upscale_sharp()); else VKLOG("present: contrast-adaptive sharpening on (DM_VK_SHARPEN %d)", sharpen_level());
    return 1;
}
static VkDescriptorSet sharpen_set(vkimg_t *im) {   // the window image's view and set (the game's window images: a few, kept)
    for (int i = 0; i < 4; i++) if (g_sh.img[i] == im->img && g_sh.view[i]) return g_sh.set[i];
    int i = g_sh.next++ % 4;
    if (g_sh.view[i]) { vkDeviceWaitIdle(V.dev); vkDestroyImageView(V.dev, g_sh.view[i], NULL); g_sh.view[i] = VK_NULL_HANDLE; }   // (an image of a window made again: rare)
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->img, VK_IMAGE_VIEW_TYPE_2D, im->fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(V.dev, &vc, NULL, &g_sh.view[i])) { g_sh.view[i] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    g_sh.img[i] = im->img;
    VkDescriptorImageInfo ii = {g_sh.samp, g_sh.view[i], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_sh.set[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(V.dev, 1, &w, 0, NULL);
    return g_sh.set[i];
}
// draws the picture sharpened into tgt (TRANSFER_DST after its clear) → 1 (tgt is then COLOR_ATTACHMENT_OPTIMAL), or 0: blit it as before
static int sharpen_draw(VkCommandBuffer cb, vkimg_t *im, VkImage tgt, VkImageView tview, uint32_t idx, int cx, int cy, int cw, int ch, int dx, int dy, int dw, int dh) {
    int lvl = sharpen_level(); float up = upscale_sharp(); if ((!lvl && !up) || im->samples > 1 || im->layers > 1) return 0;
    if (!sharpen_ready(up > 0)) return 0;
    VkFramebuffer fb = V.has_dynrender ? VK_NULL_HANDLE : ov_fb(idx); if (!V.has_dynrender && !fb) return 0;
    VkDescriptorSet set = sharpen_set(im); if (!set) return 0;
    barrier(cb, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (V.has_dynrender) { VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, tview, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, g_sext}, 1, 0, 1, &ca, NULL, NULL}; vkCmdBeginRendering(cb, &ri); }
    else { VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_ov.rp, fb, {{0, 0}, g_sext}, 0, NULL}; vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, up > 0 ? g_sh.upipe : g_sh.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_sh.layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {(float)dx, (float)dy, (float)dw, (float)dh, 0, 1}; VkRect2D sc = {{dx < 0 ? 0 : dx, dy < 0 ? 0 : dy}, {(uint32_t)dw, (uint32_t)dh}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    struct { float src[4], sharp, flip; } pc = {{(float)cx / im->w, (float)cy / im->h, (float)cw / im->w, (float)ch / im->h}, up > 0 ? up : lvl / 100.0f, vk_window_bottom_up() ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, g_sh.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (V.has_dynrender) vkCmdEndRendering(cb); else vkCmdEndRenderPass(cb);
    return 1;
}
static void present_window(vkimg_t *im, const int crop[4], double period, uint64_t ticket) {   // period: the game's frame period (s); < 0 for a redraw (ticket 0)
    uint32_t k = g_pi++ % 8;
    vkWaitForFences(V.dev, 1, &g_pfence[k], 1, UINT64_MAX);
    VkFormat want = im->fmt == VK_FORMAT_R8G8B8A8_SRGB || im->fmt == VK_FORMAT_B8G8R8A8_SRGB || im->fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (want != g_want) { g_want = want; swap_destroy(); if (!swap_make()) return; }   // once, at the first present of the other kind
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[k], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swap_destroy(); if (!swap_make()) return; r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[k], VK_NULL_HANDLE, &idx); }
    if (r < 0) { VKRED_ONCE(90, "vkAcquireNextImageKHR: %d", r); return; }
    vkResetFences(V.dev, 1, &g_pfence[k]);
    int turned = g_pre != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;   // composed into g_rot.img, then copied turned into the swapchain image
    VkImage tgt = turned ? g_rot.img : g_simg[idx]; VkImageView tview = turned ? g_rot.view : g_sview[idx];
    VkCommandBuffer cb = g_pcb[k];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vkBeginCommandBuffer(cb, &bi);
    vk_img_barrier_all(cb);   // after everything submitted before on this queue
    barrier(cb, tgt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
    int cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] ? crop[2] : (int)im->w, ch = crop && crop[3] ? crop[3] : (int)im->h;
    double s = fmin((double)g_sext.width / cw, (double)g_sext.height / ch); int dw = (int)(cw * s), dh = (int)(ch * s), dx = ((int)g_sext.width - dw) / 2, dy = ((int)g_sext.height - dh) / 2;
    vk_pic_set((float)dx / g_sext.width, (float)dy / g_sext.height, (float)dw / g_sext.width, (float)dh / g_sext.height);
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (vk_window_bottom_up()) { b.srcOffsets[0].y = cy + ch; b.srcOffsets[1].y = cy; }   // lower-left origin: the window image is stored bottom-up (gfx_vk_exec.c)
    if (im->layout == VK_IMAGE_LAYOUT_UNDEFINED) { barrier(cb, im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL); im->layout = VK_IMAGE_LAYOUT_GENERAL; }   // an imported frame buffer, first shown (gl_android.c)
    VkImageLayout lay = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (sharpen_draw(cb, im, tgt, tview, idx, cx, cy, cw, ch, dx, dy, dw, dh)) lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // (DM_VK_SHARPEN)
    else vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    int external = dm_external_active();
    int ovm = external ? 0 : dm_overlay_visible(), menu = external ? -1 : dm_menu_state(), fps = dm_overlay_fps();
    if ((ovm || menu >= 0 || fps) && overlay_ready(cb, k, menu) && (V.has_dynrender || ov_fb(idx))) {   // without dynamic rendering, only with the image's framebuffer
        barrier(cb, tgt, lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, tview, lay, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, g_sext}, 1, 0, 1, &ca, NULL, NULL};
        VkFramebuffer ofb = V.has_dynrender ? VK_NULL_HANDLE : ov_fb(idx);
        if (V.has_dynrender) vkCmdBeginRendering(cb, &ri);
        else { VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_ov.rp, ofb, {{0, 0}, g_sext}, 0, NULL};
               vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_ov.set, 0, NULL);
        VkViewport vp = {0, 0, (float)g_sext.width, (float)g_sext.height, 0, 1}; VkRect2D sc = {{0, 0}, g_sext};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        ov_render(cb, ovm, menu, fps);
        if (V.has_dynrender) vkCmdEndRendering(cb); else vkCmdEndRenderPass(cb);
    }
    if (turned) {
        barrier(cb, g_rot.img, lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cb, g_simg[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ra = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_sview[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo rri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, g_next}, 1, 0, 1, &ra, NULL, NULL};
        vkCmdBeginRendering(cb, &rri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rot.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rot.layout, 0, 1, &g_rot.set, 0, NULL);
        VkViewport vp = {0, 0, (float)g_next.width, (float)g_next.height, 0, 1}; VkRect2D sc = {{0, 0}, g_next};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        // Where each point (u, v) of the panel's image reads the window's picture: the turn the compositor would have made, a quarter turn clockwise for
        // ROTATE_90 (u, v) -> (v, 1 - u), anticlockwise for ROTATE_270 (u, v) -> (1 - v, u)
        static const float cw[6] = {0, 1, -1, 0, 0, 1}, ccw[6] = {0, -1, 1, 0, 1, 0};
        vkCmdPushConstants(cb, g_rot.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof cw, g_pre == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? cw : ccw);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        barrier(cb, g_simg[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else barrier(cb, g_simg[idx], lay, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &g_acq[k], &ws, 1, &cb, 1, &g_done_sem[k]};
    extern pthread_mutex_t g_vk_queue_mu;
    pthread_mutex_lock(&g_vk_queue_mu);
    if (vkQueueSubmit(V.q, 1, &si, g_pfence[k])) { pthread_mutex_unlock(&g_vk_queue_mu); VKRED_ONCE(91, "present submit failed"); return; }
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &g_done_sem[k], 1, &g_swap, &idx, NULL};
    VkPresentTimeGOOGLE ptm = {++g_pace_id, period >= 0 ? pace_desired(period) : 0};
    VkPresentTimesInfoGOOGLE pti = {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, NULL, 1, &ptm};
    if (V.has_dtiming) pi.pNext = &pti;
    r = vkQueuePresentKHR(V.q, &pi);
    if (V.has_dtiming && r >= 0) present_time_map(ptm.presentID, ticket);
    pthread_mutex_unlock(&g_vk_queue_mu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_destroy();
    else if (r == VK_SUBOPTIMAL_KHR && g_pre != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {   // the device turned over (ROTATE_90 <-> ROTATE_270): a swapchain for the new turn
        VkSurfaceCapabilitiesKHR c; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, g_surf, &c);
        if ((c.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != (VkSurfaceTransformFlagsKHR)g_pre) swap_destroy();
    }
}
// headless frame shots
static int shot_wanted(uint64_t n) {
    static uint64_t until; const char *k = getenv("DM_VK_SHOTS");   // a DM_CTL "env DM_VK_SHOTS=N" line: each of the next N presents (a transition caught frame by frame)
    if (k && atoi(k) > 0) { until = n + (uint64_t)atoi(k); setenv("DM_VK_SHOTS", "0", 1); VKLOG("frame shots: presents %llu to %llu", (unsigned long long)n, (unsigned long long)until - 1); }
    if (n < until) return 1;
    static const char *e; static int init; if (!init) { init = 1; e = getenv("DM_VK_SHOT"); }
    if (!e) return 0;
    for (const char *p = e; *p;) { char *end; unsigned long long v = strtoull(p, &end, 10); if (end == p) break; if (v == n) return 1; p = *end == ',' ? end + 1 : end; }
    return 0;
}
static void shot(vkimg_t *im, uint64_t n) {
    void mtl_sub_begin(void); uint64_t mtl_sub_commit(void); int mtl_wait(uint64_t, uint64_t);
    uint32_t w = im->w, h = im->h; VkBuffer sb; VkDeviceSize so;
    mtl_sub_begin();
    uint8_t *p = vk_scratch((VkDeviceSize)w * h * 4, 16, &sb, &so);
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy c = {so, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
    mtl_wait(mtl_sub_commit(), ~0ull);
    const char *dir = getenv("DM_VK_SHOT_DIR"); if (!dir) dir = getenv("DM_ROOT"); if (!dir) dir = ".";
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)n);
    FILE *f = fopen(path, "wb");
    if (!f) { VKLOG("[RED] cannot write %s", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    int bgra = im->fmt == VK_FORMAT_B8G8R8A8_UNORM || im->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    uint8_t *row = malloc((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) { const uint8_t *s = p + (size_t)y * w * 4; for (uint32_t x = 0; x < w; x++) { row[3 * x] = s[4 * x + (bgra ? 2 : 0)]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x + (bgra ? 0 : 2)]; } fwrite(row, 1, (size_t)w * 3, f); }
    free(row); fclose(f);
    VKLOG("frame %llu written to %s (%ux%u)", (unsigned long long)n, path, w, h);
}
static pthread_mutex_t g_present_mu = PTHREAD_MUTEX_INITIALIZER;   // the swapchain against the window going away (vk_window_lost, on the main thread)
void vk_window_lost(void) { pthread_mutex_lock(&g_present_mu); swap_destroy(); g_win = NULL; pthread_mutex_unlock(&g_present_mu); }
// The present thread: mtl_present only queues the frame and returns (as on Apple), so the game thread never waits in vkAcquireNextImageKHR or on the
// swapchain's fences (a FIFO swapchain blocks there until a vsync); the window texture is not drawn again before its blit, because NVN's acquire waits for
// its ticket (mtl_present_wait), and the blit's submission comes after the frame's own on the one queue. DM_VK_SYNC_PRESENT=1 presents on the game thread.
#define PQ_N 4
static struct { vkimg_t *im; int crop[4], has_crop; uint64_t n; double period; } g_pq[PQ_N];
static uint64_t g_pq_head, g_pq_tail, g_pq_done;   // queued [tail, head), presents up to ticket g_pq_done are blitted
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
// While the game is held (the pause menu) no frame comes: dm_overlay_redraw presents the last one again with what the overlay now shows.
static _Atomic int g_redraw;
void dm_overlay_redraw(void) { atomic_store(&g_redraw, 1); pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); }
static void *present_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk present");
    typeof(g_pq[0]) last = {0};
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_tail == g_pq_head && !atomic_load(&g_redraw)) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        atomic_store(&g_redraw, 0);
        if (g_pq_tail == g_pq_head) {   // a redraw, no new frame
            pthread_mutex_unlock(&g_pq_mu);
            if (last.im) { pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(last.im, last.has_crop ? last.crop : NULL, -1, 0); pthread_mutex_unlock(&g_present_mu); }
            continue;
        }
        typeof(g_pq[0]) e = g_pq[g_pq_tail % PQ_N];
        pthread_mutex_unlock(&g_pq_mu);
        if (e.im) { last = e; pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(e.im, e.has_crop ? e.crop : NULL, e.period, e.n); pthread_mutex_unlock(&g_present_mu); }
        pthread_mutex_lock(&g_pq_mu); g_pq_tail++; g_pq_done = e.n; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }
    return NULL;
}
static void present_thread_start(void) { pthread_t t; pthread_create(&t, NULL, present_thread, NULL); pthread_detach(t); }
static uint64_t present_enqueue(vkimg_t *im, const int crop[4], uint64_t n, double period) {   // im NULL: nothing to show, the ticket still completes in order
    static int sync = -1; static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (sync < 0) sync = getenv("DM_VK_SYNC_PRESENT") != NULL;
    if (sync || !dm_front_window()) {   // headless: nothing to show; or presenting on this thread
        if (sync && im) { pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(im, crop, period, n); pthread_mutex_unlock(&g_present_mu); }
        pthread_mutex_lock(&g_pq_mu); if (g_pq_tail == g_pq_head) g_pq_done = n; pthread_mutex_unlock(&g_pq_mu);   // done at once (after any queued ones)
        return n;
    }
    pthread_once(&once, present_thread_start);
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_head - g_pq_tail >= PQ_N) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // never more than PQ_N frames behind
    typeof(g_pq[0]) *e = &g_pq[g_pq_head % PQ_N]; e->im = im; e->n = n; e->period = period; e->has_crop = crop != NULL; if (crop) memcpy(e->crop, crop, sizeof e->crop);
    g_pq_head++; pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return n;
}
void dm_perf_frame(double period); void dm_perf_window(ANativeWindow *w, double fps); void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns);   // perf_android.c
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    vk_init();
    { uint64_t vk_replay_ns(void); double vk_gpu_ms_recent(void), vk_gpu_work_ms_recent(void);   // what else the frame costs: its replay and its GPU time
      // DM_ADAPT_GPU=work (start-up): the GPU's own work (the parts' spans, gfx_vk_core.c g_tsp) and not the submission's span, which holds its waits for the replay
      static int gwork = -1; if (gwork < 0) { const char *e = getenv("DM_ADAPT_GPU"); gwork = e && !strcmp(e, "work"); }
      static uint64_t last_exec; uint64_t ex = vk_replay_ns(), rp = ex - last_exec; last_exec = ex; double g = gwork ? vk_gpu_work_ms_recent() : vk_gpu_ms_recent(); int64_t gn = g > 0 ? (int64_t)(g * 1e6) : 0;
      dm_perf_frame_cost((int64_t)rp, gn); }
    dm_perf_frame(period);   // paced to the game's frame period, and its work time reported to ADPF
    // A longer period goes to the window only once it has held for 60 presents; a shorter one at once. Super Mario Bros. Wonder's own controller (main+0xcfd24) asks
    // for interval 2 for a frame or a few and then 1 again: each ask was a vote (176 in 3 minutes of its first level on the Y700, 27-60 a run in issue #64's log of an
    // Adreno 750 phone with a 60 Hz display), and where the display changes its mode for a vote, the switch itself lengthens the next frames (issue #45: 30-40 fps)
    static ANativeWindow *rate_win; static double rate_period, want_period; static uint64_t want_since;
    if (period != want_period) { want_period = period; want_since = g_presents; }
    if (g_win && (g_win != rate_win || (period != rate_period && (period < rate_period || g_presents - want_since >= 60)))) { rate_win = g_win; rate_period = period; dm_perf_window(g_win, period > 0 ? 1.0 / period : 60); }
    uint64_t n = ++g_presents;
    // without display times (no VK_GOOGLE_display_timing, no window, or none came back) the hand-over time stands in, as before: the governor's late check needs readings
    { int disp = V.has_dtiming && dm_front_window() && (n <= 240 || g_pres_recorded); struct timespec ts; if (!disp) clock_gettime(CLOCK_MONOTONIC, &ts);
      present_time_begin(n, disp ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec); }
    vkimg_t *im = vk_tex(t);
    static uint64_t no_img, undrawn, shown;
    if (n == 600) { void crash_handlers_check(const char *when); crash_handlers_check("after 600 presents"); }
    if (n % 300 == 0) { void vk_pass_report(void); vk_pass_report(); void vk_set_why_report(void); vk_set_why_report(); }   // (DM_VK_PASS_TIMES, DM_VK_SET_WHY: averages over 300 presents)
    if (n % 600 == 0) {
        extern struct { uint64_t draws, skip_prog, skip_rt, skip_pipe, passes, folded, dispatches, pipes_built, rt_kept, ns_exec, ns_desc, ns_pipe, max_pass_draws; } g_vkst; extern uint64_t g_ubd_landed, g_ubd_superseded, g_ubd_shadows, g_ubd_bytes, g_ns_submit, g_gpu_ns, g_pkg_n, g_pkg_bytes, g_pkg_ns, g_set_hits, g_set_new, g_ubd_racy, g_ns_set_build, g_ns_set_hash, g_ns_set_new, g_set_memo, g_set_memo_bad;
        static uint64_t psb, psh, psn, pe, pd, pp, ps, pb, pdr, pg, pkn, pkb, pkt, psm;
        VKLOG("replay up to present %llu: draws %llu, skipped (no program %llu, no target %llu, no pipeline %llu), passes %llu, clears folded %llu, dispatches %llu, pipelines %llu; presents shown %llu, no image %llu, never drawn %llu; uniform updates landed %llu (superseded %llu, shadows %llu, CPU reads racing a GPU write %llu), same targets kept %llu, most draws in one pass %llu",
              (unsigned long long)n, (unsigned long long)g_vkst.draws, (unsigned long long)g_vkst.skip_prog, (unsigned long long)g_vkst.skip_rt, (unsigned long long)g_vkst.skip_pipe,
              (unsigned long long)g_vkst.passes, (unsigned long long)g_vkst.folded, (unsigned long long)g_vkst.dispatches, (unsigned long long)g_vkst.pipes_built,
              (unsigned long long)shown, (unsigned long long)no_img, (unsigned long long)undrawn, (unsigned long long)g_ubd_landed, (unsigned long long)g_ubd_superseded, (unsigned long long)g_ubd_shadows, (unsigned long long)g_ubd_racy, (unsigned long long)g_vkst.rt_kept, (unsigned long long)g_vkst.max_pass_draws);
        double nd = g_vkst.draws > pdr ? (double)(g_vkst.draws - pdr) / 600 : 0;
        VKLOG("replay per frame (last 600): %.0f draws, replay %.2f ms (descriptor sets %.2f, pipeline keys %.2f), queue submit %.2f ms, uniform shadows %.0f KB; GPU busy %.2f ms; texture uploads %.1f (%.2f MB, %.2f ms); descriptor sets reused %llu, new %llu (collecting %.2f ms, hashing %.2f, new ones %.2f), kept per stage %.0f a frame (memo differences %llu)",
              nd, (g_vkst.ns_exec - pe) / 6e8, (g_vkst.ns_desc - pd) / 6e8, (g_vkst.ns_pipe - pp) / 6e8, (g_ns_submit - ps) / 6e8, (g_ubd_bytes - pb) / 600.0 / 1024, (g_gpu_ns - pg) / 6e8, (g_pkg_n - pkn) / 600.0, (g_pkg_bytes - pkb) / 600.0 / 1048576, (g_pkg_ns - pkt) / 6e8, (unsigned long long)g_set_hits, (unsigned long long)g_set_new, (g_ns_set_build - psb) / 6e8, (g_ns_set_hash - psh) / 6e8, (g_ns_set_new - psn) / 6e8,
              (g_set_memo - psm) / 600.0, (unsigned long long)g_set_memo_bad);
        { extern uint64_t g_order_kept, g_order_ended; static uint64_t pk, pn;   // gfx_vk_exec.c g_order_keep
          if (g_order_kept + g_order_ended > pk + pn) VKLOG("order-only barriers per frame (last 600): %.1f kept inside their pass, %.1f ended it", (g_order_kept - pk) / 600.0, (g_order_ended - pn) / 600.0);
          pk = g_order_kept; pn = g_order_ended; }
        pe = g_vkst.ns_exec; pd = g_vkst.ns_desc; pp = g_vkst.ns_pipe; ps = g_ns_submit; pb = g_ubd_bytes; pdr = g_vkst.draws; pg = g_gpu_ns; pkn = g_pkg_n; pkb = g_pkg_bytes; pkt = g_pkg_ns; psb = g_ns_set_build; psh = g_ns_set_hash; psn = g_ns_set_new; psm = g_set_memo;
    }
    if (!im) { no_img++; return present_enqueue(NULL, NULL, n, period); }
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL) { undrawn++; return present_enqueue(NULL, NULL, n, period); }   // never drawn into: nothing to show yet
    shown++; atomic_fetch_add(&g_fps_frames, 1);
    if (shot_wanted(n)) shot(im, n);
    { void vk_pass_dump_poll(uint64_t done); uint64_t vk_fence_done(void); vk_pass_dump_poll(vk_fence_done()); void vk_pass_frame(void); vk_pass_frame(); void vk_tex_sweep(void); vk_tex_sweep(); }
    if (n % 1800 == 0) {   // every ~30 s: the pipeline cache to disk on a thread of its own (an Android app can be killed without any exit path)
        void vk_pipeline_cache_save(void); pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &a, (void *(*)(void *))(void *)vk_pipeline_cache_save, NULL); pthread_attr_destroy(&a);
    }
    return present_enqueue(im, crop, n, period);
}
// OpenGL ES games (gl_android.c): their frames are AHardwareBuffers the device's GL driver drew, imported as images and shown as an NVN window texture is
vkimg_t *vk_image_from_ahb(AHardwareBuffer *ahb, uint32_t w, uint32_t h) {
    vk_init();
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    if (vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap)) { VKRED_ONCE(92, "vkGetAndroidHardwareBufferPropertiesANDROID failed for a GL frame buffer"); return NULL; }
    VkExternalMemoryImageCreateInfo ei = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ei, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    vkimg_t *im = calloc(1, sizeof *im);
    if (vkCreateImage(V.dev, &ci, NULL, &im->img)) { VKRED_ONCE(93, "vkCreateImage for a GL frame buffer failed"); free(im); return NULL; }
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, ahb};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, im->img, VK_NULL_HANDLE};
    uint32_t mt = 0; while (mt < 32 && !(ap.memoryTypeBits & (1u << mt))) mt++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, ap.allocationSize, mt};
    if (vkAllocateMemory(V.dev, &ai, NULL, &im->mem) || vkBindImageMemory(V.dev, im->img, im->mem, 0)) { VKRED_ONCE(94, "importing a GL frame buffer into Vulkan failed");
        if (im->mem) vkFreeMemory(V.dev, im->mem, NULL); vkDestroyImage(V.dev, im->img, NULL); free(im); return NULL; }   // (tried again at the next frame: nothing of this one stays)
    im->fmt = VK_FORMAT_R8G8B8A8_UNORM; im->w = w; im->h = h; im->d = im->layers = im->levels = im->samples = 1; im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKLOG("GL frame buffer %ux%u imported (memory type %u, %llu bytes)", w, h, mt, (unsigned long long)ap.allocationSize);
    return im;
}
uint64_t vk_present_vkimg(vkimg_t *im, double period) {
    vk_init();
    dm_perf_frame(period);
    uint64_t n = ++g_presents;
    { int disp = V.has_dtiming && dm_front_window() && (n <= 240 || g_pres_recorded); struct timespec ts; if (!disp) clock_gettime(CLOCK_MONOTONIC, &ts);
      present_time_begin(n, disp ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec); }
    atomic_fetch_add(&g_fps_frames, 1);
    if (shot_wanted(n) && im->layout == VK_IMAGE_LAYOUT_GENERAL) shot(im, n);
    return present_enqueue(im, NULL, n, period);
}
void mtl_present_wait(uint64_t tk) {   // NVN acquire: the window texture's last present has been blitted (the game may draw into it again)
    if (!tk) return;
    pthread_mutex_lock(&g_pq_mu);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
    while (g_pq_done < tk && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &dl) == 0) {}
    pthread_mutex_unlock(&g_pq_mu);
}
double mtl_presented_time(uint64_t n) { return present_time_get(n); }
double vk_refresh_ms(void) { return V.has_dtiming && g_pres_recorded && g_refresh_ns ? g_refresh_ns / 1e6 : 0; }   // perf_android.c's late check: the display's refresh, when display times are what it gets
uint32_t mtl_present_qos(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_has_window(void) { return dm_front_window() != NULL; }
int mtl_display_hz(void) { return 60; }
int mtl_fps_target(void) { return DM_GAME_FPS_TARGET > 0 ? DM_GAME_FPS_TARGET : 60; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_prog_release(prog_t *p) { (void)p; }
