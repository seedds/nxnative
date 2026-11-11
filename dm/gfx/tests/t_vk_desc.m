// Vulkan descriptors outlive the objects written into them (GitHub issue 68): a set names an image view, a sampler and a uniform buffer in element 1 of an 8-texture
// array and a buffer binding, as Super Mario Galaxy 2's shaders take them; the objects are destroyed and their host structs freed and overwritten, and the set bound
// for a draw still binds the texture, sampler state and buffer it was written with. Resetting the pool releases them
#include "../vk/hle_vk.m"
#include VK_INPUT_H
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

static void integer_vertices(VkDevice dev) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:vk_input_test_source options:nil error:&error];
    CHECK(library != nil, "translated integer vertex test shader compiles");
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    id<MTLLibrary> saved_library = g_lib; g_lib = library;
    struct VkShaderModule_T module = {.key = "input", .fn = [library newFunctionWithName:@"vk_input"]};
    VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = &module, .pName = "main"};
    // A Vulkan SpecId that coincides with a host constant must not override it.
    uint32_t poison = 31; VkSpecializationMapEntry entry = {0, 0, sizeof poison};
    VkSpecializationInfo spec = {1, &entry, sizeof poison, &poison}; stage.pSpecializationInfo = &spec;
    VkVertexInputBindingDescription bindings[2] = {{0,4,VK_VERTEX_INPUT_RATE_VERTEX}, {1,4,VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attrs[2] = {{0,0,VK_FORMAT_R16_UINT,0}, {1,1,VK_FORMAT_R16_SINT,0}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = VK_TRUE};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering, .stageCount = 1, .pStages = &stage, .pVertexInputState = &vi, .pRasterizationState = &raster};
    const struct { VkFormat a, b; uint32_t bits, signed_a; } cases[] = {
        {VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,16,0}, {VK_FORMAT_R16_SINT,VK_FORMAT_R16_UINT,16,1},
        {VK_FORMAT_R8_SINT,VK_FORMAT_R8_UINT,8,1}, {VK_FORMAT_R32_SINT,VK_FORMAT_R32_UINT,32,1}
    };
    for (size_t n = 0; n < sizeof cases / sizeof cases[0]; n++) {
        attrs[0].format = cases[n].a; attrs[1].format = cases[n].b;
        VkPipeline p = pipe_make(&ci); CHECK(p && p->pso, "integer vertex pipeline accepts independently signed source formats");
        if (!p || !p->pso) { if (p) h_vkDestroyPipeline(dev, p, NULL); continue; }
        uint32_t bits = cases[n].bits, mask = UINT32_MAX >> (32 - bits);
        uint32_t input[3] = {0, (1u << (bits - 1)) | 1u, mask};
        id<MTLBuffer> data = [g_dev newBufferWithBytes:input length:sizeof input options:MTLResourceStorageModeShared];
        id<MTLBuffer> result = [g_dev newBufferWithLength:6 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.renderTargetWidth = rp.renderTargetHeight = rp.defaultRasterSampleCount = 1;
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:p->pso]; [enc setVertexBuffer:data offset:0 atIndex:30]; [enc setVertexBuffer:data offset:0 atIndex:29]; [enc setVertexBuffer:result offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:3]; [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
        int ok = cb.status == MTLCommandBufferStatusCompleted; uint32_t *out = result.contents;
        for (int i = 0; i < 3; i++) {
            uint32_t signed_value = input[i] & mask;
            if (bits < 32 && (signed_value & (1u << (bits - 1)))) signed_value |= ~mask;
            uint32_t a = cases[n].signed_a ? signed_value : input[i], b = cases[n].signed_a ? input[i] : signed_value;
            if (out[2*i] != a || out[2*i+1] != b) ok = 0;
        }
        CHECK(ok, "GPU vertex fetch zero/sign extension preserves high-bit values in signed and unsigned shader inputs");
        h_vkDestroyPipeline(dev, p, NULL);
    }
    g_lib = saved_library;
}

int main(void) {
    __weak id<MTLTexture> wtex; __weak id<MTLSamplerState> wsmp; __weak id<MTLBuffer> wbuf;
    VkDevice dev = &g_device; VkDescriptorPool pool; VkDescriptorSet ds; VkDescriptorSetLayout L;
    @autoreleasepool {
        dev_init();
        VkFormatProperties old_format;
        VkFormatProperties2 format2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        h_vkGetPhysicalDeviceFormatProperties(&g_pdev, VK_FORMAT_R8G8B8A8_UNORM, &old_format);
        h_vkGetPhysicalDeviceFormatProperties2(&g_pdev, VK_FORMAT_R8G8B8A8_UNORM, &format2);
        CHECK(!memcmp(&old_format, &format2.formatProperties, sizeof old_format), "promoted format query preserves base capabilities");
        CHECK(vk_lookup("vkGetPhysicalDeviceFormatProperties2KHR") == (void *)h_vkGetPhysicalDeviceFormatProperties2, "KHR format query resolves to its promoted core entry");
        VkQueue queue1, queue2;
        VkDeviceQueueInfo2 queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        h_vkGetDeviceQueue(dev, 0, 0, &queue1); h_vkGetDeviceQueue2(dev, &queue_info, &queue2);
        CHECK(queue1 == queue2, "Vulkan 1.1 queue query returns the same queue");
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {4, 4, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkImage im; CHECK(h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS, "image created");
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                     .components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A},   // a view texture of its own, not the image's
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView view; CHECK(h_vkCreateImageView(dev, &vci, NULL, &view) == VK_SUCCESS && view->tex && view->tex != im->tex, "swizzled view has its own texture");
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR};
        VkSampler smp; CHECK(h_vkCreateSampler(dev, &sci, NULL, &smp) == VK_SUCCESS && smp->s, "sampler created");
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer buf; h_vkCreateBuffer(dev, &bci, NULL, &buf);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = 1};
        VkDeviceMemory mem; h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(h_vkBindBufferMemory(dev, buf, mem, 512) == VK_SUCCESS, "buffer bound");
        { // Two synchronization2 submits must preserve both command buffers and signal the final fence.
            VkBuffer source; VkBufferCreateInfo src_info = bci; src_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            h_vkCreateBuffer(dev, &src_info, NULL, &source); h_vkBindBufferMemory(dev, source, mem, 0);
            uint8_t *bytes = mem->buf.contents; for (int k = 0; k < 64; k++) bytes[k] = (uint8_t)(k * 3);
            VkCommandPool command_pool; VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            h_vkCreateCommandPool(dev, &pool_info, NULL, &command_pool);
            VkQueryPool queries; VkQueryPoolCreateInfo query_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
            h_vkCreateQueryPool(dev, &query_info, NULL, &queries);
            const uint8_t checker[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255}; memcpy(bytes + 512 + 192, checker, sizeof checker);
            VkImage gradient; VkImageCreateInfo gradient_info = ici; gradient_info.extent = (VkExtent3D){2,2,1}; gradient_info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; h_vkCreateImage(dev, &gradient_info, NULL, &gradient);
            VkImage multisample; VkImageCreateInfo msaa_info = ici; msaa_info.samples = VK_SAMPLE_COUNT_4_BIT;
            h_vkCreateImage(dev, &msaa_info, NULL, &multisample);
            VkImageView render_view; VkImageViewCreateInfo render_vci = vci; render_vci.image = multisample; render_vci.components = (VkComponentMapping){0};
            h_vkCreateImageView(dev, &render_vci, NULL, &render_view);
            NSError *shader_error = nil;
            id<MTLLibrary> test_library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; struct V { float4 p [[position]]; }; vertex V test_vertex(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return V{float4(p[i],0,1)}; } fragment float4 test_fragment() { return float4(1); } kernel void test_compute(device uint* out [[buffer(0)]], constant uint& add [[buffer(1)]], uint id [[thread_position_in_grid]]) { out[id] = id * 7 + add; }" options:nil error:&shader_error];
            CHECK(test_library != nil, "test raster shaders compile");
            struct VkShaderModule_T vertex_module = {.fn = [test_library newFunctionWithName:@"test_vertex"]}, fragment_module = {.fn = [test_library newFunctionWithName:@"test_fragment"]};
            VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = &vertex_module, .pName = "main"},
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = &fragment_module, .pName = "main"}};
            VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering_pipeline = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format};
            VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
            VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT};
            VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR, .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .colorWriteMask = 15};
            VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
            VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
            VkPipelineDynamicStateCreateInfo dynamics = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dynamic_states};
            VkPipelineLayout pipeline_layout; VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; h_vkCreatePipelineLayout(dev, &layout_info, NULL, &pipeline_layout);
            VkGraphicsPipelineCreateInfo graphics = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering_pipeline, .stageCount = 2, .pStages = stages,
                .pInputAssemblyState = &assembly, .pRasterizationState = &raster, .pMultisampleState = &samples, .pColorBlendState = &blending, .pDynamicState = &dynamics, .layout = pipeline_layout};
            VkPipeline pipeline; CHECK(h_vkCreateGraphicsPipelines(dev, NULL, 1, &graphics, NULL, &pipeline) == VK_SUCCESS && pipeline->pso, "dynamic-rendering graphics pipeline compiles without a render-pass object");
            shinfo compute_info = {.key = "t_compute", .compute = 1, .local = {4,1,1}, .nres = 2, .res = {{'b',0,0,1,3}, {'p',0,1,1,0}}};
            struct VkShaderModule_T compute_module = {.fn = [test_library newFunctionWithName:@"test_compute"], .si = &compute_info};
            VkDescriptorSetLayoutBinding storage_binding = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
            VkDescriptorSetLayoutCreateInfo storage_layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &storage_binding};
            VkDescriptorSetLayout storage_layout; h_vkCreateDescriptorSetLayout(dev, &storage_layout_info, NULL, &storage_layout);
            VkPushConstantRange push_range = {VK_SHADER_STAGE_ALL, 0, 4};
            VkPipelineLayoutCreateInfo compute_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &storage_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
            VkPipelineLayout compute_layout; h_vkCreatePipelineLayout(dev, &compute_layout_info, NULL, &compute_layout);
            VkComputePipelineCreateInfo compute_pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = &compute_module, .pName = "main"}, .layout = compute_layout};
            VkPipeline compute_pipeline; CHECK(h_vkCreateComputePipelines(dev, NULL, 1, &compute_pipeline_info, NULL, &compute_pipeline) == VK_SUCCESS, "compute pipeline takes its reflected local size");
            VkDescriptorPool compute_pool; VkDescriptorPoolCreateInfo compute_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2}; h_vkCreateDescriptorPool(dev, &compute_pool_info, NULL, &compute_pool);
            VkDescriptorSet compute_sets[2]; VkDescriptorSetLayout storage_layouts[2] = {storage_layout, storage_layout};
            VkDescriptorSetAllocateInfo compute_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = compute_pool, .descriptorSetCount = 2, .pSetLayouts = storage_layouts}; h_vkAllocateDescriptorSets(dev, &compute_allocate, compute_sets);
            VkDescriptorBufferInfo storage_buffers[2] = {{buf,224,32},{buf,0,32}}; VkWriteDescriptorSet storage_writes[2] = {0};
            for (int i = 0; i < 2; i++) storage_writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = compute_sets[i], .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &storage_buffers[i]};
            h_vkUpdateDescriptorSets(dev, 2, storage_writes, 0, NULL);
            VkBuffer indirect; VkBufferCreateInfo indirect_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 12, .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT}; h_vkCreateBuffer(dev, &indirect_info, NULL, &indirect); h_vkBindBufferMemory(dev, indirect, mem, 256);
            uint32_t group_counts[3] = {2,1,1}; memcpy(bytes + 256, group_counts, sizeof group_counts);
            VkCommandBuffer commands[2]; VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool, .commandBufferCount = 2};
            h_vkAllocateCommandBuffers(dev, &alloc, commands);
            VkCommandBufferSubmitInfo command_info[2] = {0}; VkSubmitInfo2 submits[2] = {0};
            for (int k = 0; k < 2; k++) {
                VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; h_vkBeginCommandBuffer(commands[k], &begin);
                VkBufferCopy copy = {(VkDeviceSize)k * 32, (VkDeviceSize)k * 32, 32}; h_vkCmdCopyBuffer(commands[k], source, buf, 1, &copy);
                if (!k) {
                    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = render_view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = {.25f, .5f, .75f, 1.f}};
                    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {4, 4}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
                    h_vkCmdBeginRendering(commands[k], &rendering);
                    h_vkCmdBindPipeline(commands[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    float constants[4] = {.1f, .1f, .1f, 1.f}; h_vkCmdSetBlendConstants(commands[k], constants); h_vkCmdDraw(commands[k], 3, 1, 0, 0);
                    constants[0] = .25f; constants[1] = .5f; constants[2] = .75f; h_vkCmdSetBlendConstants(commands[k], constants); h_vkCmdDraw(commands[k], 3, 1, 0, 0);
                    constants[0] = 0; h_vkCmdEndRendering(commands[k]);
                    VkImageResolve resolve = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {4, 4, 1}};
                    h_vkCmdResolveImage(commands[k], multisample, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
                    VkBufferImageCopy2 upload_region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, .bufferOffset = 192, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {2,2,1}};
                    VkCopyBufferToImageInfo2 upload = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, .srcBuffer = buf, .dstImage = gradient, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .regionCount = 1, .pRegions = &upload_region}; h_vkCmdCopyBufferToImage2(commands[k], &upload);
                    VkImageBlit scaled = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .srcOffsets = {{0,0,0},{2,2,1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .dstOffsets = {{0,0,0},{1,1,1}}};
                    h_vkCmdBlitImage(commands[k], gradient, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &scaled, VK_FILTER_LINEAR);
                    VkImageBlit2 mirrored = {.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .srcOffsets = {{2,0,0},{0,2,1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .dstOffsets = {{2,2,0},{4,4,1}}};
                    VkBlitImageInfo2 mirror_info = {.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .srcImage = gradient, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .dstImage = im, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .regionCount = 1, .pRegions = &mirrored, .filter = VK_FILTER_NEAREST}; h_vkCmdBlitImage2(commands[k], &mirror_info);
                    VkBufferImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, .bufferOffset = 64, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
                    VkCopyImageToBufferInfo2 readback = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, .srcImage = im, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .dstBuffer = buf, .regionCount = 1, .pRegions = &region};
                    h_vkCmdCopyImageToBuffer2(commands[k], &readback);
                    attachment.clearValue.color.float32[0] = 0; region.imageExtent.width = 0; // recordings own their input values
                    h_vkCmdWriteTimestamp2(commands[k], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
                    h_vkCmdCopyQueryPoolResults(commands[k], queries, 0, 1, buf, 128, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                    VkBufferCopy query_copy = {128, 160, 16}; h_vkCmdCopyBuffer(commands[k], buf, buf, 1, &query_copy);
                }
                if (k == 1) {
                    h_vkCmdBindPipeline(commands[k], VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
                    h_vkCmdBindDescriptorSets(commands[k], VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1, &compute_sets[0], 0, NULL);
                    h_vkCmdBindDescriptorSets(commands[k], VK_PIPELINE_BIND_POINT_GRAPHICS, compute_layout, 0, 1, &compute_sets[1], 0, NULL);
                    uint32_t add = 5, graphics_add = 99; h_vkCmdPushConstants(commands[k], compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
                    h_vkCmdPushConstants(commands[k], compute_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &graphics_add); h_vkCmdDispatch(commands[k], 2, 1, 1);
                    VkBufferCopy compute_snapshot = {224,64,32}; h_vkCmdCopyBuffer(commands[k], buf, source, 1, &compute_snapshot);
                    add = 9; h_vkCmdPushConstants(commands[k], compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add); h_vkCmdDispatchIndirect(commands[k], indirect, 0);
                }
                h_vkEndCommandBuffer(commands[k]);
                command_info[k] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = commands[k]};
                submits[k] = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command_info[k]};
            }
            VkFence fence; VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; h_vkCreateFence(dev, &fence_info, NULL, &fence);
            CHECK(h_vkQueueSubmit2(queue2, 2, submits, fence) == VK_SUCCESS, "synchronization2 submits both command buffers");
            CHECK(h_vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS && !memcmp(bytes, bytes + 512, 64), "submit2 fence covers both GPU buffer copies");
            uint32_t direct_values[8], indirect_values[8]; memcpy(direct_values, bytes + 64, 32); memcpy(indirect_values, bytes + 512 + 224, 32);
            int compute_ok = 1; for (int i = 0; i < 8; i++) if (direct_values[i] != (uint32_t)i * 7 + 5 || indirect_values[i] != (uint32_t)i * 7 + 9) compute_ok = 0;
            CHECK(compute_ok, "direct/indirect compute dispatch preserves compute descriptors and push constants across graphics binds");
            h_vkDestroyDescriptorPool(dev, compute_pool, NULL); h_vkDestroyDescriptorSetLayout(dev, storage_layout, NULL); h_vkDestroyPipeline(dev, compute_pipeline, NULL); h_vkDestroyPipelineLayout(dev, compute_layout, NULL); h_vkDestroyBuffer(dev, indirect, NULL);
            uint64_t copied_stamp = 0, copied_availability = 0; memcpy(&copied_stamp, bytes + 512 + 160, 8); memcpy(&copied_availability, bytes + 512 + 168, 8);
            CHECK(copied_stamp && copied_availability == 1, "query result and availability reach a later GPU copy before the fence");
            uint8_t *scaled_pixel = bytes + 512 + 64; CHECK(scaled_pixel[0] >= 127 && scaled_pixel[0] <= 128 && scaled_pixel[1] >= 127 && scaled_pixel[1] <= 128 && scaled_pixel[2] >= 127 && scaled_pixel[2] <= 128 && scaled_pixel[3] == 255, "linear blit averages four source texels");
            uint8_t *mirrored_pixel = bytes + 512 + 64 + 40; CHECK(mirrored_pixel[0] == 0 && mirrored_pixel[1] == 255 && mirrored_pixel[2] == 0 && mirrored_pixel[3] == 255, "nearest CopyCommands2 blit reverses the source X coordinates");
            uint8_t *pixel = bytes + 512 + 64 + 4;
            CHECK(pixel[0] >= 63 && pixel[0] <= 64 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] >= 191 && pixel[2] <= 192 && pixel[3] == 255, "dynamic blend changes, multisample resolve and CopyImageToBuffer2 produce the expected pixels");
            h_vkDestroyImageView(dev, render_view, NULL); h_vkDestroyImage(dev, multisample, NULL); h_vkDestroyImage(dev, gradient, NULL); h_vkDestroyPipeline(dev, pipeline, NULL); h_vkDestroyPipelineLayout(dev, pipeline_layout, NULL);
            h_vkDestroyQueryPool(dev, queries, NULL);
            h_vkDestroyFence(dev, fence, NULL); h_vkFreeCommandBuffers(dev, command_pool, 2, commands); h_vkDestroyCommandPool(dev, command_pool, NULL); h_vkDestroyBuffer(dev, source, NULL);
        }
        wtex = view->tex; wsmp = smp->s; wbuf = mem->buf;

        VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
        h_vkCreateDescriptorSetLayout(dev, &lci, NULL, &L);
        VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1};
        h_vkCreateDescriptorPool(dev, &pci, NULL, &pool);
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &L};
        h_vkAllocateDescriptorSets(dev, &ai, &ds);
        VkDescriptorImageInfo ii = {smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi = {buf, 64, 128};
        VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .dstArrayElement = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi}};
        h_vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

        // The game destroys everything the set names, then the freed host structs are reused (the allocator's free-list word, then other objects' data)
        h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL); h_vkDestroySampler(dev, smp, NULL); h_vkDestroyBuffer(dev, buf, NULL); h_vkFreeMemory(dev, mem, NULL);
        void *junk[4096];
        for (int i = 0; i < 4096; i++) { size_t n = 8 + 8 * (i % 8); junk[i] = malloc(n); memset(junk[i], 0xa5, n); }
        CHECK(wtex && wsmp && wbuf, "the descriptors keep the texture, sampler state and buffer alive");
        const desc_t *e = &ds->d[L->b[L->idx[0]].base + 1], *u = &ds->d[L->b[L->idx[1]].base];
        CHECK(e->tex == wtex && e->smp == wsmp, "element 1 holds the view's texture and the sampler state");
        CHECK(u->buf == wbuf && u->off == 512 + 64, "the buffer descriptor holds the memory's buffer at the buffer's offset plus its own");

        // A fragment shader taking c:0.0=0*8 and u:0.1=0, bound for a draw as pre_draw does
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = [g_dev newTextureWithDescriptor:td]; rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        shinfo si = {.key = "t_vk_desc", .frag = 1, .nres = 2, .res = {{'c', 0, 0, 8, 0}, {'u', 0, 0, 1, 1}}};
        rs_t s = {0}; s.cb = [g_q commandBuffer]; s.re = [s.cb renderCommandEncoderWithDescriptor:rpd]; s.set[0].s = ds;
        bind_res(&s, &si, 1);
        [s.re endEncoding]; s.re = nil; [s.cb commit]; [s.cb waitUntilCompleted];
        CHECK(s.cb.status == MTLCommandBufferStatusCompleted, "the bound pass completed");
        s.cb = nil;
        for (int i = 0; i < 4096; i++) free(junk[i]);
    }
    @autoreleasepool { h_vkResetDescriptorPool(dev, pool, 0); }
    CHECK(!wtex && !wsmp && !wbuf, "resetting the pool releases what its sets held");
    // A stencil-aspect view of a depth-stencil image without the mutable-format flag (PR #106): an X32_Stencil8 view, which Metal's validation allows only with the format-view usage
    @autoreleasepool {
        VkFormat dsf[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        for (int i = 0; i < 2; i++) {
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = dsf[i], .extent = {4, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
            VkImage im; CHECK(h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS && (im->tex.usage & MTLTextureUsagePixelFormatView), "a depth-stencil image has the format-view usage");
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dsf[i],
                                         .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
            VkImageView sv; CHECK(h_vkCreateImageView(dev, &vci, NULL, &sv) == VK_SUCCESS && sv->tex.pixelFormat == MTLPixelFormatX32_Stencil8 && sv->tex.parentTexture == im->tex, "its stencil-aspect view is a stencil-only view of it");
            h_vkDestroyImageView(dev, sv, NULL); h_vkDestroyImage(dev, im, NULL);
        }
    }
    integer_vertices(dev);
    printf("Vulkan descriptors: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
