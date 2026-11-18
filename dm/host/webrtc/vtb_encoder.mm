#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <IOSurface/IOSurface.h>
#include "vtb_encoder.h"
#include <vector>
#include <atomic>
#include <mutex>

struct vtb_encoder {
    VTCompressionSessionRef session = nullptr;
    CVPixelBufferPoolRef pool = nullptr;
    OSType cv_format = 0;
    int width = 854;
    int height = 480;
    int fps = 60;
    int bitrate_kbps = 1500;
    vtb_nalu_callback_t callback = nullptr;
    void *user_ctx = nullptr;
    std::atomic<bool> force_keyframe{true};
    std::mutex session_lock;
};

static void vtb_output_callback(void *outputCallbackRefCon,
                                void *sourceFrameRefCon,
                                OSStatus status,
                                VTEncodeInfoFlags infoFlags,
                                CMSampleBufferRef sampleBuffer) {
    vtb_encoder_t *enc = (vtb_encoder_t *)outputCallbackRefCon;
    uint64_t pts_ns = (uint64_t)(uintptr_t)sourceFrameRefCon;

    if (status != noErr || !sampleBuffer) return;

    // Detect keyframe
    bool is_keyframe = false;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
        is_keyframe = (!not_sync || !CFBooleanGetValue(not_sync));
    }

    const uint8_t annexb_prefix[4] = {0, 0, 0, 1};
    std::vector<uint8_t> frame_buffer;

    // Insert SPS & PPS on keyframe
    if (is_keyframe) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (format) {
            size_t param_count = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &param_count, NULL) == noErr) {
                for (size_t i = 0; i < param_count; i++) {
                    const uint8_t *param = NULL;
                    size_t param_len = 0;
                    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &param, &param_len, NULL, NULL) == noErr && param && param_len > 0) {
                        frame_buffer.insert(frame_buffer.end(), annexb_prefix, annexb_prefix + 4);
                        frame_buffer.insert(frame_buffer.end(), param, param + param_len);
                    }
                }
            }
        }
    }

    // Extract NALUs from CMSampleBuffer (AVCC format: [4-byte length][nalu]...)
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (blockBuffer) {
        size_t total_length = 0;
        char *data_pointer = NULL;
        if (CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &total_length, &data_pointer) == noErr && data_pointer) {
            size_t offset = 0;
            while (offset + 4 < total_length) {
                uint32_t nalu_len = CFSwapInt32BigToHost(*(const uint32_t *)(data_pointer + offset));
                offset += 4;
                if (offset + nalu_len <= total_length) {
                    frame_buffer.insert(frame_buffer.end(), annexb_prefix, annexb_prefix + 4);
                    frame_buffer.insert(frame_buffer.end(), (const uint8_t *)data_pointer + offset, (const uint8_t *)data_pointer + offset + nalu_len);
                    offset += nalu_len;
                } else {
                    break;
                }
            }
        }
    }

    if (!frame_buffer.empty() && enc->callback) {
        enc->callback(enc->user_ctx, frame_buffer.data(), frame_buffer.size(), is_keyframe, pts_ns);
    }
}

static bool vtb_init_session_locked(vtb_encoder_t *enc, int width, int height, OSType cv_format) {
    if (enc->session) {
        VTCompressionSessionInvalidate(enc->session);
        CFRelease(enc->session);
        enc->session = NULL;
    }
    if (enc->pool) {
        CVPixelBufferPoolRelease(enc->pool);
        enc->pool = NULL;
    }

    width = (width / 2) * 2;
    height = (height / 2) * 2;
    if (width < 320) width = 320;
    if (height < 180) height = 180;

    enc->width = width;
    enc->height = height;

    NSDictionary *pixelBufferAttributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(cv_format),
        (id)kCVPixelBufferWidthKey: @(width),
        (id)kCVPixelBufferHeightKey: @(height),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferMetalCompatibilityKey: @YES
    };

    NSDictionary *poolAttributes = @{
        (id)kCVPixelBufferPoolMinimumBufferCountKey: @4
    };

    CVReturn pool_ret = CVPixelBufferPoolCreate(kCFAllocatorDefault,
                                                (__bridge CFDictionaryRef)poolAttributes,
                                                (__bridge CFDictionaryRef)pixelBufferAttributes,
                                                &enc->pool);
    if (pool_ret != kCVReturnSuccess || !enc->pool) {
        NSLog(@"[VTB] CVPixelBufferPoolCreate failed: %d", (int)pool_ret);
        return false;
    }

    NSDictionary *encoderSpec = @{
        (id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl: @YES
    };

    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault,
                                             width, height,
                                             kCMVideoCodecType_H264,
                                             (__bridge CFDictionaryRef)encoderSpec,
                                             (__bridge CFDictionaryRef)pixelBufferAttributes,
                                             NULL,
                                             vtb_output_callback,
                                             enc,
                                             &enc->session);
    if (st != noErr || !enc->session) {
        NSLog(@"[VTB] VTCompressionSessionCreate failed: %d", (int)st);
        return false;
    }

    // Ultra-low-latency real-time properties
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse); // Zero B-frames

    int max_delay = 0;
    CFNumberRef delay_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &max_delay);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_MaxFrameDelayCount, delay_num);
    CFRelease(delay_num);

    int target_bitrate = enc->bitrate_kbps * 1000;
    CFNumberRef br_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &target_bitrate);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AverageBitRate, br_num);
    CFRelease(br_num);

    // Rate Limiting: Limit burst bitrate to 1.5x average per 1.0 second to prevent Wi-Fi UDP microburst drops
    int bytes_per_sec = (int)(((int64_t)target_bitrate * 3) / (2 * 8));
    double window_sec = 1.0;
    CFNumberRef bytes_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bytes_per_sec);
    CFNumberRef sec_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &window_sec);
    const void *limits_vals[2] = { bytes_num, sec_num };
    CFArrayRef limits_arr = CFArrayCreate(kCFAllocatorDefault, limits_vals, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_DataRateLimits, limits_arr);
    CFRelease(limits_arr);
    CFRelease(bytes_num);
    CFRelease(sec_num);

    int max_key_interval = enc->fps * 2; // 2s GOP
    CFNumberRef kf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &max_key_interval);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kf_num);
    CFRelease(kf_num);

    CFNumberRef fps_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &enc->fps);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ExpectedFrameRate, fps_num);
    CFRelease(fps_num);

    VTCompressionSessionPrepareToEncodeFrames(enc->session);
    enc->force_keyframe.store(true, std::memory_order_relaxed);
    NSLog(@"[VTB] VideoToolbox H.264 Encoder ready: %dx%d @ %dfps (%d kbps)", width, height, enc->fps, enc->bitrate_kbps);
    return true;
}

vtb_encoder_t *vtb_encoder_create(int width, int height, int fps, int bitrate_kbps, vtb_nalu_callback_t cb, void *user) {
    if (width <= 0 || height <= 0 || !cb) return NULL;

    vtb_encoder_t *enc = new vtb_encoder();
    enc->fps = fps > 0 ? fps : 60;
    enc->bitrate_kbps = bitrate_kbps > 0 ? bitrate_kbps : 3500;
    enc->callback = cb;
    enc->user_ctx = user;

    if (!vtb_init_session_locked(enc, width, height, kCVPixelFormatType_32BGRA)) {
        delete enc;
        return NULL;
    }
    return enc;
}

void vtb_encoder_destroy(vtb_encoder_t *enc) {
    if (!enc) return;
    std::lock_guard<std::mutex> lock(enc->session_lock);
    if (enc->session) {
        VTCompressionSessionInvalidate(enc->session);
        CFRelease(enc->session);
        enc->session = NULL;
    }
    if (enc->pool) {
        CVPixelBufferPoolRelease(enc->pool);
        enc->pool = NULL;
    }
    delete enc;
}

void vtb_encoder_request_keyframe(vtb_encoder_t *enc) {
    if (enc) enc->force_keyframe.store(true, std::memory_order_relaxed);
}

int vtb_encoder_encode_metal(vtb_encoder_t *enc, void *mtl_tex_ptr, void *mtl_dev_ptr, void *mtl_cmd_queue_ptr, uint64_t pts_ns) {
    if (!enc || !mtl_tex_ptr || !mtl_dev_ptr || !mtl_cmd_queue_ptr) return -1;
    id<MTLTexture> src_tex = (__bridge id<MTLTexture>)mtl_tex_ptr;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)mtl_dev_ptr;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_cmd_queue_ptr;

    // Detect format
    OSType cv_format = kCVPixelFormatType_32BGRA;
    MTLPixelFormat target_mtl_format = MTLPixelFormatBGRA8Unorm;
    if (src_tex.pixelFormat == MTLPixelFormatRGBA8Unorm || src_tex.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
        cv_format = kCVPixelFormatType_32RGBA;
        target_mtl_format = MTLPixelFormatRGBA8Unorm;
    }

    std::unique_lock<std::mutex> lock(enc->session_lock);

    // Initialize or reconfigure if session not created or format changed
    if (!enc->session || !enc->pool || enc->cv_format != cv_format) {
        if (!vtb_init_session_locked(enc, enc->width, enc->height, cv_format)) {
            return -2;
        }
        enc->cv_format = cv_format;
    }

    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn cv_ret = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, enc->pool, &pixelBuffer);
    if (cv_ret != kCVReturnSuccess || !pixelBuffer) return -3;

    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
    if (!surface) {
        CVPixelBufferRelease(pixelBuffer);
        return -4;
    }

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target_mtl_format
                                                                                    width:enc->width
                                                                                   height:enc->height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> dst_tex = [dev newTextureWithDescriptor:desc iosurface:surface plane:0];
    if (!dst_tex) {
        CVPixelBufferRelease(pixelBuffer);
        return -5;
    }

    // Source view without sRGB if needed so blit/scale doesn't reject format mismatch
    id<MTLTexture> blit_src = src_tex;
    if (src_tex.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB) {
        blit_src = [src_tex newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    } else if (src_tex.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
        blit_src = [src_tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    }

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (blit_src.width == (NSUInteger)enc->width && blit_src.height == (NSUInteger)enc->height) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (blit) {
            [blit copyFromTexture:blit_src
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(enc->width, enc->height, 1)
                        toTexture:dst_tex
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }
    } else {
        MPSImageBilinearScale *scale = [[MPSImageBilinearScale alloc] initWithDevice:dev];
        [scale encodeToCommandBuffer:cb sourceTexture:blit_src destinationTexture:dst_tex];
    }

    CVPixelBufferRetain(pixelBuffer);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (buffer.status != MTLCommandBufferStatusCompleted) {
            CVPixelBufferRelease(pixelBuffer);
            return;
        }

        std::lock_guard<std::mutex> enc_lock(enc->session_lock);
        if (!enc->session) {
            CVPixelBufferRelease(pixelBuffer);
            return;
        }

        static std::atomic<uint64_t> s_last_pts{0};
        uint64_t last = s_last_pts.load(std::memory_order_relaxed);
        uint64_t actual_pts = pts_ns;
        if (actual_pts <= last) {
            actual_pts = last + 1000000;
        }
        s_last_pts.store(actual_pts, std::memory_order_relaxed);

        CMTime pts = CMTimeMake(actual_pts / 1000, 1000000); // Microseconds monotonic
        NSDictionary *frameProps = nil;
        static std::atomic<int> s_frame_count{0};
        bool force_kf = enc->force_keyframe.exchange(false, std::memory_order_relaxed);
        // Force keyframe on demand or at least once every 60 frames (1 second) to guarantee new/reconnecting clients always receive an IDR frame
        if (force_kf || (s_frame_count.fetch_add(1) % 60 == 0)) {
            frameProps = @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES};
        }

        VTCompressionSessionEncodeFrame(enc->session,
                                        pixelBuffer,
                                        pts,
                                        kCMTimeInvalid,
                                        (__bridge CFDictionaryRef)frameProps,
                                        (void *)(uintptr_t)actual_pts,
                                        NULL);
        CVPixelBufferRelease(pixelBuffer);
    }];
    [cb commit];
    CVPixelBufferRelease(pixelBuffer);
    return 0;
}
