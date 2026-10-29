// Tests of the Vulkan backend's device layer (dm/gfx/hostvk/vk_device.c; docs/design/vulkan-backend.md 3.6, 9.1, 10 item 3), headless, under the validation layer: hostvk_init
// meets the baseline on the device it picks; the level and the optional extensions and dynamic states the device lacks are printed; hostvk_caps is filled; destroy and
// re-create; an explicit device_index, and one out of range (a clean error, nothing left behind); a wanted surface (the front end's instance extensions, VK_KHR_swapchain), and a
// front end without a Vulkan window. On Linux CTest runs it on lavapipe (dm/host/CMakeLists.txt); with no Vulkan at all it skips (t_vk_common.h)
#include "t_vk_common.h"

static void print_caps(const hostvk_dev *d) {
    const hostvk_caps *c = &d->caps;
    printf("device %u '%s' (type %d, vendor 0x%04x), Vulkan %u.%u.%u used at %u.%u, driver '%s' '%s'\n", d->device_index, d->props.deviceName, (int)d->props.deviceType,
           d->props.vendorID, VK_API_VERSION_MAJOR(d->props.apiVersion), VK_API_VERSION_MINOR(d->props.apiVersion), VK_API_VERSION_PATCH(d->props.apiVersion),
           VK_API_VERSION_MAJOR(d->api_version), VK_API_VERSION_MINOR(d->api_version), d->driver_name, d->driver_info);
    printf("loader %u.%u.%u; queue family %u, transfer-only family %s\n", VK_API_VERSION_MAJOR(d->instance_version), VK_API_VERSION_MINOR(d->instance_version),
           VK_API_VERSION_PATCH(d->instance_version), d->queue_family, d->transfer_family == UINT32_MAX ? "none" : "present");
    printf("level %c: gpl %u eds3_blend %u eds3_raster %u vertex_input_dynamic %u cond_render %u xfb %u geometry %u tessellation %u astc_ldr %u logic_op %u unified_memory %u\n",
           c->level, c->gpl, c->eds3_blend, c->eds3_raster, c->vertex_input_dynamic, c->cond_render, c->xfb, c->geometry, c->tessellation, c->astc_ldr, c->logic_op,
           c->unified_memory);
    printf("max_samples %u max_point_size %u max_vertex_attribs %u timeline %u buffer_device_address %u\n", c->max_samples, c->max_point_size, c->max_vertex_attribs,
           c->timeline, c->buffer_device_address);
    printf("min_import_alignment %llu max_buffer_size %llu max_allocations %u import_type_bits 0x%x import_type %u host_coherent_import %u\n",
           (unsigned long long)c->min_import_alignment, (unsigned long long)c->max_buffer_size, c->max_allocations, c->import_type_bits, c->import_type, c->host_coherent_import);
    printf("optional (design 9.1) missing: %s\n", d->missing[0] ? d->missing : "none");
    printf("dynamic states of level A missing: %s\n", d->eds_missing[0] ? d->eds_missing : "none");
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
        printf("memory type %u: heap %u flags 0x%x%s\n", i, d->mem.memoryTypes[i].heapIndex, d->mem.memoryTypes[i].propertyFlags,
               (c->import_type_bits >> i & 1) ? " (host memory can be imported as it)" : "");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hostvk_dev d;
    char err[1024], name[256];
    hostvk_opts o = {-1, 1, 0};

    // ---- First device that meets the baseline ----
    int rc = init_or_skip("t_vk_device", &o, &d, err, sizeof err);
    snprintf(name, sizeof name, "hostvk_init picks a device that meets the baseline (rc %d%s%s)", rc, err[0] ? ": " : "", err);
    t(name, rc == 0);
    if (rc) { check_validation(&d, "failed init"); return finish("t_vk_device"); }
    print_caps(&d);
    const hostvk_caps *c = &d.caps;
    t("instance, device and queue made", d.instance && d.phys && d.device && d.queue);
    t("the device is used at Vulkan 1.3 or later", d.api_version >= VK_API_VERSION_1_3);
    snprintf(name, sizeof name, "level is one of A-D (%c)", c->level);
    t(name, c->level >= 'A' && c->level <= 'D');
    t("level follows the rules of vk_device.h", c->level == (c->gpl ? (c->vertex_input_dynamic && c->eds3_blend && c->eds3_raster ? 'A' : 'B')
                                                           : (c->vertex_input_dynamic || d.eds_present ? 'C' : 'D')));
    t("baseline capabilities set: geometry, tessellation, logic_op, xfb, timeline", c->geometry && c->tessellation && c->logic_op && c->xfb && c->timeline);
    t("min_import_alignment is a power of two", c->min_import_alignment && !(c->min_import_alignment & (c->min_import_alignment - 1)));
    t("max_allocations, max_buffer_size, max_vertex_attribs (>= 16), max_point_size, max_samples set",
      c->max_allocations > 0 && c->max_buffer_size >= c->min_import_alignment && c->max_vertex_attribs >= 16 && c->max_point_size >= 1 && c->max_samples >= 1);
    t("host memory can be imported as some memory type, the preferred one among them",
      c->import_type_bits && c->import_type < d.mem.memoryTypeCount && (c->import_type_bits >> c->import_type & 1));
    t("host_coherent_import matches the preferred type's flags",
      c->host_coherent_import == ((d.mem.memoryTypes[c->import_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0));
    t("missing lists agree with the bits", (d.missing[0] != 0) == (!d.vk14 || !d.astc_ldr || d.opt_present != (1u << HOSTVK_OPT_N) - 1) &&
                                          (d.eds_missing[0] != 0) == (d.eds_present != (1u << HOSTVK_EDS_N) - 1));
    t("no swapchain without a surface", !d.swapchain);
    t("no allocation live after init", d.allocations == 0);
    if (d.validation) {   // the messenger sees a deliberate error, so "no validation message" below means something
        uint32_t e0 = d.val_errors;
        printf("expected: one validation error for a buffer with no usage bit\n");
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, 0, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer b = VK_NULL_HANDLE;
        if (vkCreateBuffer(d.device, &bi, NULL, &b) == VK_SUCCESS) vkDestroyBuffer(d.device, b, NULL);
        snprintf(name, sizeof name, "the messenger counts a deliberate validation error (%u)", (unsigned)(d.val_errors - e0));
        t(name, d.val_errors > e0);
        d.val_errors = e0;
    }
    char first_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]; snprintf(first_name, sizeof first_name, "%s", d.props.deviceName);
    uint32_t first_index = d.device_index; char first_level = c->level;
    hostvk_destroy(&d);
    t("destroy releases the handles", !d.instance && !d.device && !d.messenger && !d.queue);
    check_validation(&d, "first device");

    // ---- Destroy and re-create, then the same device by its index ----
    rc = hostvk_init(&o, &d, err, sizeof err);
    t("re-created after destroy", rc == 0 && !strcmp(d.props.deviceName, first_name) && d.caps.level == first_level);
    if (!rc) { check_validation(&d, "re-created"); hostvk_destroy(&d); }
    o.device_index = (int)first_index;
    rc = hostvk_init(&o, &d, err, sizeof err);
    t("device_index of the first device picks it", rc == 0 && d.device_index == first_index && !strcmp(d.props.deviceName, first_name));
    if (!rc) { check_validation(&d, "device_index"); hostvk_destroy(&d); }

    // ---- device_index out of range: a clean error, nothing made ----
    o.device_index = 4096;
    rc = hostvk_init(&o, &d, err, sizeof err);
    printf("device_index 4096: rc %d: %s\n", rc, err);
    t("device_index out of range fails with HOSTVK_E_INDEX and a reason", rc == HOSTVK_E_INDEX && strstr(err, "out of range"));
    t("and leaves no Vulkan object", !d.instance && !d.device && !d.messenger);
    check_validation(&d, "device_index out of range");
    o.device_index = -1;

    // ---- A surface wanted: the front end's instance extensions, VK_KHR_swapchain in the baseline ----
    g_front_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME; g_front_n = 1;
    o.want_surface = 1;
    rc = hostvk_init(&o, &d, err, sizeof err);
    snprintf(name, sizeof name, "want_surface with VK_KHR_surface: init (rc %d%s%s), VK_KHR_swapchain enabled", rc, err[0] ? ": " : "", err);
    t(name, rc == 0 && d.swapchain);
    if (!rc) { check_validation(&d, "want_surface"); hostvk_destroy(&d); }
    // A front end without a Vulkan window gives no extension: a clean error
    g_front_n = 0;
    rc = hostvk_init(&o, &d, err, sizeof err);
    printf("want_surface without a Vulkan window: rc %d: %s\n", rc, err);
    t("want_surface without a Vulkan window fails cleanly", rc == HOSTVK_E_VULKAN && !d.instance);
    // An instance extension the loader does not have: a clean error before vkCreateInstance
    g_front_exts[0] = "VK_KHR_no_such_surface"; g_front_n = 1;
    rc = hostvk_init(&o, &d, err, sizeof err);
    printf("want_surface with an unknown extension: rc %d: %s\n", rc, err);
    t("an unknown front-end extension fails cleanly", rc == HOSTVK_E_VULKAN && strstr(err, "VK_KHR_no_such_surface") && !d.instance);
    return finish("t_vk_device");
}
