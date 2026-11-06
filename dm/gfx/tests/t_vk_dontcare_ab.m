// DM_VK_DONTCARE=ab:<m>,<m>[,...] with DM_AB=<frames> runs the DONT_CARE modes of the Vulkan layer in turn in one process (frame f is in phase f / DM_AB, phase p runs mode
// list[p % n]) and prints one "[ab] phase" line per phase, in the format dm/tools/gpuprof.py --ab reads. Here a frame is a submission of two passes that open a depth image
// cleared to 0.25 with DONT_CARE and STORE and draw nothing (under MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1 a DontCare load fills the image with a pattern, so the readback tells the
// mode: 0.25 for all, not 0.25 for none), the present follows the submission before it has completed, and the image is read back in the same command buffer. Checked:
// the list and DM_AB are validated (a malformed list, one mode, more than four, or no DM_AB use the default policy); every submission's passes run the mode of the frame it was made
// in, also when g_frames has moved on before the replay (a replay with an older frame tag than g_frames); the phases' [ab] lines come out one per completed phase with the right
// configuration, GPU time, frame time and attachment traffic, and gpuprof.py's parser reads each of them
#include "../vk/hle_vk.m"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

enum { W = 512, H = 512, NIM = 2, FRAMES = 20, LEN = 2 };
static VkImage g_im[NIM]; static VkDeviceMemory g_rb; static VkCommandBuffer g_cb;

// A command buffer that clears NIM depth images to 0.25, opens each in a pass with DONT_CARE and STORE and ends it, then copies it into the read-back buffer at image * W * H * 4
static void build_scene(VkDevice dev) {
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);
    VkAttachmentDescription ad = {0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &ar};
    VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
    VkRenderPass rp; h_vkCreateRenderPass(dev, &rci, NULL, &rp);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)NIM * W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf; h_vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bci.size, .memoryTypeIndex = 1};
    h_vkAllocateMemory(dev, &mai, NULL, &g_rb);
    if (h_vkBindBufferMemory(dev, buf, g_rb, 0) != VK_SUCCESS) { printf("read-back buffer not bound\n"); abort(); }
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    h_vkAllocateCommandBuffers(dev, &cai, &g_cb);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(g_cb, &cbi);
    for (int i = 0; i < NIM; i++) {
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .extent = {W, H, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        h_vkCreateImage(dev, &ici, NULL, &g_im[i]);
        if (!g_im[i]) { printf("depth image not created\n"); abort(); }
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_im[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                                     .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        VkImageView view; h_vkCreateImageView(dev, &vci, NULL, &view);
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1};
        VkFramebuffer fb; h_vkCreateFramebuffer(dev, &fci, NULL, &fb);
        VkClearDepthStencilValue quarter = {0.25f, 0}; VkImageSubresourceRange dsr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        h_vkCmdClearDepthStencilImage(g_cb, g_im[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &quarter, 1, &dsr);
        VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}};
        h_vkCmdBeginRenderPass(g_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        h_vkCmdEndRenderPass(g_cb);
        VkBufferImageCopy bc = {.bufferOffset = (VkDeviceSize)i * W * H * 4, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        h_vkCmdCopyImageToBuffer(g_cb, g_im[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
    }
    h_vkEndCommandBuffer(g_cb);
}
// Texels of image i the last submission left at 0.25
static int kept(int i) {
    const float *p = (const float *)((const uint8_t *)g_rb->buf.contents + (size_t)i * W * H * 4); int n = 0;
    for (int k = 0; k < W * H; k++) n += p[k] == 0.25f;
    return n;
}
// The state a run starts from: the environment as given and the policy not yet read
static void set_env(const char *list, const char *len) {
    if (list) setenv("DM_VK_DONTCARE", list, 1); else unsetenv("DM_VK_DONTCARE");
    if (len) setenv("DM_AB", len, 1); else unsetenv("DM_AB");
    atomic_store(&g_vk_dontcare, -1); g_dc_ab_n = 0; g_dc_ab_len = 0;
}
static int mode_phase(uint64_t f) { return (f / LEN) % 2 ? DC_ALL : DC_NONE; }   // the run's list is none,all: not the layer's own mapping

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        // The switch's validation: only a list of two to four known modes with DM_AB gives the A/B policy, anything else the default
        static const struct { const char *list, *len; int mode; } bad[] = {
            {"ab:none,all", NULL}, {"ab:none,all", "0"}, {"ab:none,all", "-3"}, {"ab:none,all", "2junk"}, {"ab:none,all", ""}, {"ab:none,bogus", "2"}, {"ab:none", "2"}, {"ab:", "2"}, {"ab:none,,all", "2"}, {"ab", "2"},
            {"ab:none,all,none,all,none", "2"}, {"ab:none,all,", "2"}, {"ab:none,default", "2"}, {"fast", "2"}, {"default", "2"},
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            set_env(bad[i].list, bad[i].len);
            char msg[128]; snprintf(msg, sizeof msg, "DM_VK_DONTCARE=%s DM_AB=%s uses the default policy", bad[i].list, bad[i].len ? bad[i].len : "(unset)");
            CHECK(dontcare_mode() == DC_ALL, msg);
        }
        set_env("colour", "2"); CHECK(dontcare_mode() == DC_COLOUR, "DM_VK_DONTCARE=colour is unchanged by DM_AB");
        set_env("none", "2"); CHECK(dontcare_mode() == DC_NONE, "DM_VK_DONTCARE=none is unchanged by DM_AB");
        set_env("all", "2"); CHECK(dontcare_mode() == DC_ALL, "DM_VK_DONTCARE=all is unchanged by DM_AB");
        set_env(NULL, "2"); CHECK(dontcare_mode() == DC_ALL, "unset is the default policy, all");
        set_env("ab:all,none,colour,none", "3");
        CHECK(dontcare_mode() == DC_AB && g_dc_ab_n == 4 && g_dc_ab_len == 3, "four modes and DM_AB=3 give the A/B policy");
        static const int want4[] = {DC_ALL, DC_NONE, DC_COLOUR, DC_NONE};
        int map_ok = 1;
        for (uint64_t f = 0; f < 60; f++) map_ok &= dc_pass_mode(DC_AB, f) == want4[(f / 3) % 4];
        CHECK(map_ok, "frame f runs mode list[(f / DM_AB) % n]");

        set_env("ab:none,all", "2");   // the run below: phases of LEN frames, none then all
        CHECK(dontcare_mode() == DC_AB && g_dc_ab_n == 2 && g_dc_ab_len == LEN, "ab:none,all with DM_AB=2 gives the A/B policy");
        dev_init();
        build_scene(dev);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cb};

        // A replay tagged with an older frame than g_frames (the present happened between the submission and its replay) takes the tag's mode in both passes
        for (int t = 0; t < 4; t++) {
            uint64_t tag = (uint64_t)t, now = tag + LEN;   // g_frames has moved on into the next phase, which runs the other mode
            atomic_store(&g_frames, now);
            rs_t rs = {0}; subq_t sq = {0}; sq.frame = tag; rs.cb = [g_q commandBuffer]; rs.sq = &sq; replay(&rs, g_cb);
            [rs.cb commit]; [rs.cb waitUntilCompleted];
            int want = mode_phase(tag) == DC_ALL, other = mode_phase(now) == DC_ALL;
            char msg[160]; snprintf(msg, sizeof msg, "a replay tagged frame %d, g_frames %d: both passes take the tag's mode (%s)", t, (int)now, want ? "all" : "none");
            CHECK(want != other && kept(0) == (want ? W * H : 0) && kept(1) == (want ? W * H : 0), msg);
        }
        atomic_store(&g_frames, 0);
        for (int k = 0; k < DC_RING; k++) { atomic_store(&g_dc_ld[k], 0); atomic_store(&g_dc_st[k], 0); atomic_store(&g_dc_np[k], 0); atomic_store(&g_dc_gpu[k], 0); }

        // The run: FRAMES frames, each a submission, then the present before the submission has completed, then the wait and the readback. The log of the run is kept
        char path[] = "/tmp/t_vk_dontcare_ab.XXXXXX"; int fd = mkstemp(path);
        if (fd < 0) { printf("no log file\n"); abort(); }
        int kept0[FRAMES], kept1[FRAMES], tagged[FRAMES]; uint64_t ld[FRAMES], st[FRAMES], np[FRAMES];
        fflush(stderr); int saved = dup(2); dup2(fd, 2);
        for (int f = 0; f < FRAMES; f++) {
            tagged[f] = atomic_load(&g_frames) == (uint64_t)f;
            h_vkQueueSubmit(&g_queue, 1, &si, NULL);
            ld[f] = atomic_load(&g_dc_ld[f % DC_RING]); st[f] = atomic_load(&g_dc_st[f % DC_RING]); np[f] = atomic_load(&g_dc_np[f % DC_RING]);   // before frame_end hands the slot on
            frame_end();
            h_vkQueueWaitIdle(&g_queue);
            kept0[f] = kept(0); kept1[f] = kept(1);
        }
        fflush(stderr); dup2(saved, 2); close(saved); close(fd);

        for (int f = 0; f < FRAMES; f++) {
            int all = mode_phase((uint64_t)f) == DC_ALL; char msg[160];
            snprintf(msg, sizeof msg, "frame %d (phase %d, %s): the frame number was current at the submission", f, f / LEN, all ? "all" : "none"); CHECK(tagged[f], msg);
            snprintf(msg, sizeof msg, "frame %d (phase %d, %s): both passes read %s", f, f / LEN, all ? "all" : "none", all ? "0.25" : "the fill pattern");
            CHECK(kept0[f] == (all ? W * H : 0) && kept1[f] == (all ? W * H : 0), msg);
            if (kept0[f] != (all ? W * H : 0) || kept1[f] != (all ? W * H : 0)) fprintf(stderr, "  frame %d: %d and %d of %d texels 0.25\n", f, kept0[f], kept1[f], W * H);
            snprintf(msg, sizeof msg, "frame %d (%s): the passes' attachment traffic is counted by the mode (load %llu store %llu passes %llu)", f, all ? "all" : "none",
                     (unsigned long long)ld[f], (unsigned long long)st[f], (unsigned long long)np[f]);
            CHECK(ld[f] == (all ? (uint64_t)NIM * W * H * 4 : 0) && st[f] == (uint64_t)NIM * W * H * 4 && np[f] == NIM, msg);
        }

        // The [ab] lines, read back by gpuprof.py's own parser: phase, configuration, frames, load, store and passes per frame, GPU and frame times
        static const char py_src[] = "import sys; sys.path.insert(0, '../../tools'); import gpuprof\n"
            "for p in gpuprof.parse_ab(open(sys.argv[1], errors='replace')): print(p['phase'], p['config'], p['frames'], p['load_mb'], p['store_mb'], p['passes'], p['gpu_p50'] > 0, p['gpu_mean'] > 0, p['frame_p50'] > 0)";
        char opath[] = "/tmp/t_vk_dontcare_ab_py.XXXXXX"; int ofd = mkstemp(opath), pst = -1; pid_t pid;
        if (ofd < 0) { printf("no output file\n"); abort(); }
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa); posix_spawn_file_actions_adddup2(&fa, ofd, 1);
        char *const pargv[] = {"python3", (char *)"-c", (char *)py_src, path, NULL};
        if (posix_spawnp(&pid, "python3", &fa, NULL, pargv, environ) == 0) waitpid(pid, &pst, 0);
        posix_spawn_file_actions_destroy(&fa); close(ofd);
        CHECK(WIFEXITED(pst) && WEXITSTATUS(pst) == 0, "gpuprof.py's parser ran (from dm/gfx/tests)");
        FILE *py = fopen(opath, "r"); int lines = 0, ok = 1; char row[256];
        // The last phase is reported when the first of the next one has its GPU time, DC_AB_LATE frames after its end: the 20 frames complete phases 0 to 4
        while (py && fgets(row, sizeof row, py)) {
            int ph, nfr; char cfg[64], g1[8], g2[8], g3[8]; double lmb, smb, pas;
            if (sscanf(row, "%d %63s %d %lf %lf %lf %7s %7s %7s", &ph, cfg, &nfr, &lmb, &smb, &pas, g1, g2, g3) != 9) { ok = 0; fprintf(stderr, "  unparsed row: %s", row); continue; }
            int all = mode_phase((uint64_t)ph * LEN) == DC_ALL;
            char want[64]; snprintf(want, sizeof want, "VK_DONTCARE=%s", all ? "all" : "none");
            // frame 0 has no frame time (nothing to measure from), so phase 0 has one frame fewer
            ok &= ph == lines && !strcmp(cfg, want) && nfr == (ph == 0 ? 1 : LEN) && lmb == (all ? 2.0 : 0.0) && smb == 2.0 && pas == 2.0 && !strcmp(g1, "True") && !strcmp(g2, "True") && !strcmp(g3, "True");
            if (!ok) fprintf(stderr, "  row %d: %s", lines, row);
            lines++;
        }
        if (py) fclose(py);
        unlink(opath);
        CHECK(lines == 5, "one [ab] line per completed phase (five of them after 20 frames)");
        CHECK(ok, "each [ab] line parses with gpuprof.py, with the phase's configuration, frames, traffic and times");
        FILE *lf = fopen(path, "r");   // the run's [ab] lines are shown; the whole log when a check failed
        for (char ln[512]; lf && fgets(ln, sizeof ln, lf);) if ((lines != 5 || !ok) || strstr(ln, "[ab] phase")) fputs(ln, stdout);
        if (lf) fclose(lf);
        unlink(path);
    }
    printf("Vulkan DONT_CARE A/B: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
