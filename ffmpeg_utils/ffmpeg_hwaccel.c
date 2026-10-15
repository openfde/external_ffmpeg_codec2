#define DEBUG_HWACCEL 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "ffmpeg_hwaccel.h"
#include "libavutil/hwcontext_vaapi.h"
#include "libavutil/opt.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const VAProfile kAv1[]   = {VAProfileAV1Profile0, VAProfileAV1Profile1};
static const VAProfile kH263[]  = {VAProfileH263Baseline};
static const VAProfile kH264[]  = {VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High};
static const VAProfile kHevc[]  = {VAProfileHEVCMain, VAProfileHEVCMain10};
static const VAProfile kMpeg2[] = {VAProfileMPEG2Simple, VAProfileMPEG2Main};
static const VAProfile kMpeg4[] = {VAProfileMPEG4Main, VAProfileMPEG4Simple, VAProfileMPEG4AdvancedSimple};
static const VAProfile kVp8[]   = {VAProfileVP8Version0_3};
static const VAProfile kVp9[]   = {VAProfileVP9Profile0, VAProfileVP9Profile1, VAProfileVP9Profile2, VAProfileVP9Profile3};

struct CodecProfileMap {
    enum AVCodecID id;
    const VAProfile *profiles;
    size_t count;
};

static const struct CodecProfileMap kCodecProfileMap[] = {
    {AV_CODEC_ID_AV1,        kAv1,   ARRAY_SIZE(kAv1)},
    {AV_CODEC_ID_H263,       kH263,  ARRAY_SIZE(kH263)},
    {AV_CODEC_ID_H264,       kH264,  ARRAY_SIZE(kH264)},
    {AV_CODEC_ID_HEVC,       kHevc,  ARRAY_SIZE(kHevc)},
    {AV_CODEC_ID_MPEG2VIDEO, kMpeg2, ARRAY_SIZE(kMpeg2)},
    {AV_CODEC_ID_MPEG4,      kMpeg4, ARRAY_SIZE(kMpeg4)},
    {AV_CODEC_ID_VP8,        kVp8,   ARRAY_SIZE(kVp8)},
    {AV_CODEC_ID_VP9,        kVp9,   ARRAY_SIZE(kVp9)},
};

int ffmpeg_hwaccel_init(AVCodecContext *avctx) {
    //if (avctx->codec_id != AV_CODEC_ID_HEVC || !property_get_bool("persist.ffmpeg_codec2.v4l2.h265", 0))
       // return 0;

    // Find codec information. At this point, AVCodecContext.codec may not be
    // set yet, so retrieve our own version using AVCodecContext.codec_id.
    const AVCodec* codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        ALOGE("ffmpeg_hwaccel_init: codec not found = %d", avctx->codec_id);
        return 0;
    }

    // Find a working HW configuration for this codec.
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            // No more HW configs available.
            break;
        }

        // Try to initialize HW device.
        if (av_hwdevice_ctx_create(&avctx->hw_device_ctx, config->device_type, NULL, NULL, 0) < 0) {
            // Initialization failed, skip this HW config.
            ALOGD_IF(DEBUG_HWACCEL, "ffmpeg_hwaccel_init: failed to initialize HW device %s",
                     av_hwdevice_get_type_name(config->device_type));
            continue;
        }

        // Use refcounted frames.
        av_opt_set_int(avctx, "refcounted_frames", 1, 0);
        // Don't use multithreading.
        avctx->thread_count = 1;

        // HW device created, stop here.
        ALOGD("ffmpeg_hwaccel_init: %p [%s], hw device = %s", avctx, codec->name,
              av_hwdevice_get_type_name(config->device_type));
        break;
    }

    if (!avctx->hw_device_ctx) {
        ALOGD("ffmpeg_hwaccel_init: no HW accel found for codec = %s", codec->name);
    }

    return 0;
}

void ffmpeg_hwaccel_deinit(AVCodecContext *avctx __unused) {
}

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx __unused, AVFrame *frame) {
    if (!frame->hw_frames_ctx) {
        // Frame is not hw-accel
        return 0;
    }

    AVFrame* output;
    int err;

    output = av_frame_alloc();
    if (!output) {
        return AVERROR(ENOMEM);
    }

    output->format = AV_PIX_FMT_NV12;

    err = av_hwframe_transfer_data(output, frame, 0);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to transfer data: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    err = av_frame_copy_props(output, frame);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to copy frame properties: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, output);
    av_frame_free(&output);

    return 0;

fail:
    av_frame_free(&output);
    return err;
}

static bool vaapi_profile_supported(AVBufferRef* deviceRef, enum AVCodecID id) {
    const VAProfile* wanted = NULL;
    int nWanted = 0;
    for (size_t i = 0; i < ARRAY_SIZE(kCodecProfileMap); i++) {
        if (kCodecProfileMap[i].id == id) {
            wanted = kCodecProfileMap[i].profiles;
            nWanted = kCodecProfileMap[i].count;
            break;
        }
    }
    if (wanted == NULL) {
        return false;
    }

    AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)deviceRef->data;
    AVVAAPIDeviceContext* vaCtx  = (AVVAAPIDeviceContext*)deviceCtx->hwctx;
    VADisplay display = vaCtx ? vaCtx->display : NULL;
    if (display == NULL) {
        return false;
    }

    int maxProfiles = vaMaxNumProfiles(display);
    VAProfile* supported = (VAProfile*)av_malloc_array(maxProfiles, sizeof(VAProfile));
    if (!supported) {
        return false;
    }

    bool found = false;
    int numProfiles = 0;
    if (vaQueryConfigProfiles(display, supported, &numProfiles) != VA_STATUS_SUCCESS) {
        av_free(supported);
        return false;
    }

    int maxEntries = vaMaxNumEntrypoints(display);
    VAEntrypoint* entries = (VAEntrypoint*)av_malloc_array(maxEntries, sizeof(VAEntrypoint));
    if (!entries) {
        goto OUT;
    }
    for (int i = 0; i < numProfiles; i++) {
        for (int j = 0; j < nWanted; j++) {
            if (supported[i] != wanted[j]) continue;
            int numEntries = 0;
            if (vaQueryConfigEntrypoints(display, supported[i], entries, &numEntries)
                == VA_STATUS_SUCCESS) {
                for (int k = 0; k < numEntries; k++) {
                    if (entries[k] == VAEntrypointVLD) {
                        found = true;
                        goto OUT;
                    }
                }
            }
        }
    }
OUT:
    if (entries) {
        av_free(entries);
    }
    av_free(supported);
    return found;
}

int ffmpeg_hwaccel_is_available(enum AVCodecID codecId) {
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        ALOGW("probe: no decoder for codec id %d", codecId);
        return 0;
    }

    int nConfigs = 0;
    int nVaapi = 0;
    for (int i = 0;;i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        nConfigs++;
        ALOGD("probe %s: hw_config[%d] type=%s methods=0x%x", codec->name, i,
              av_hwdevice_get_type_name(config->device_type), config->methods);

        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            || (config->device_type != AV_HWDEVICE_TYPE_VAAPI)) {
            continue;
        }
        nVaapi++;

        AVBufferRef* deviceRef = NULL;
        int err = av_hwdevice_ctx_create(&deviceRef, config->device_type, NULL, NULL, 0);
        if (err < 0) {
            ALOGD("probe %s: create VA-API device failed: %s", codec->name, av_err2str(err));
            continue;
        }
        const bool profileOk = vaapi_profile_supported(deviceRef, codecId);
        av_buffer_unref(&deviceRef);
        if (profileOk) {
            ALOGW("probe %s: VA-API device OK, profile_supported", codec->name);
            return 1;
        }
    }
    return 0;
}

static int vaapi_surface_limits(
        AVBufferRef* deviceRef, const VAProfile* profiles, size_t nProfiles,
        int32_t* maxWidth, int32_t* maxHeight) {
    AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)deviceRef->data;
    AVVAAPIDeviceContext* vaCtx  = (AVVAAPIDeviceContext*)deviceCtx->hwctx;
    VADisplay display = vaCtx ? vaCtx->display : NULL;
    if (display == NULL) {
        return 0;
    }

    for (size_t p = 0; p < nProfiles; p++) {
        VAConfigID configId = VA_INVALID_ID;
        if (vaCreateConfig(display, profiles[p], VAEntrypointVLD, NULL, 0, &configId)
                != VA_STATUS_SUCCESS) {
            continue;
        }

        unsigned int numAttribs = 0;
        VAStatus vas = vaQuerySurfaceAttributes(display, configId, NULL, &numAttribs);
        if (vas != VA_STATUS_SUCCESS || numAttribs == 0) {
            ALOGD("vaapi_surface_limits: profile 0x%x: count query failed (0x%x, %u)",
                  profiles[p], vas, numAttribs);
            vaDestroyConfig(display, configId);
            continue;
        }

        VASurfaceAttrib* attribs =
                (VASurfaceAttrib*) av_malloc_array(numAttribs, sizeof(VASurfaceAttrib));
        if (attribs == NULL) {
            vaDestroyConfig(display, configId);
            continue;
        }

        unsigned int numFilled = numAttribs;
        int ok = 0;
        if (vaQuerySurfaceAttributes(display, configId, attribs, &numFilled)
                == VA_STATUS_SUCCESS) {
            int32_t minWidth = 0;
            int32_t minHeight = 0;
            for (unsigned int i = 0; i < numFilled; i++) {
                switch (attribs[i].type) {
                case VASurfaceAttribMinWidth:
                    minWidth = attribs[i].value.value.i;
                    break;
                case VASurfaceAttribMaxWidth:
                    *maxWidth = attribs[i].value.value.i;
                    ok = 1;
                    break;
                case VASurfaceAttribMinHeight:
                    minHeight = attribs[i].value.value.i;
                    break;
                case VASurfaceAttribMaxHeight:
                    *maxHeight = attribs[i].value.value.i;
                    ok = 1;
                    break;
                default:
                    break;
                }
            }
            ALOGD("vaapi_surface_limits: profile 0x%x, %u attribs, %dx%d - %dx%d",
                  profiles[p], numFilled, minWidth, minHeight, *maxWidth, *maxHeight);
        } else {
            ALOGD("vaapi_surface_limits: profile 0x%x: vaQuerySurfaceAttributes failed",
                  profiles[p]);
        }
        av_free(attribs);
        vaDestroyConfig(display, configId);
        if (ok) {
            return 1;
        }
    }
    return 0;
}

int ffmpeg_hwaccel_limits(enum AVCodecID codecId, int32_t* maxWidth, int32_t* maxHeight) {
    if (maxWidth) {
        *maxWidth = 0;
    }
    if (maxHeight) {
        *maxHeight = 0;
    }

    const VAProfile* profiles = NULL;
    size_t nProfiles = 0;
    for (size_t i = 0; i < ARRAY_SIZE(kCodecProfileMap); i++) {
        if (kCodecProfileMap[i].id == codecId) {
            profiles = kCodecProfileMap[i].profiles;
            nProfiles = kCodecProfileMap[i].count;
            break;
        }
    }
    if (profiles == NULL) {
        return 0;
    }

    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        return 0;
    }

    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            || (config->device_type != AV_HWDEVICE_TYPE_VAAPI)) {
            continue;
        }

        AVBufferRef* deviceRef = NULL;
        if (av_hwdevice_ctx_create(&deviceRef, config->device_type, NULL, NULL, 0) < 0) {
            continue;
        }
        int32_t width = 0;
        int32_t height = 0;
        const int ok = vaapi_surface_limits(deviceRef, profiles, nProfiles, &width, &height);
        av_buffer_unref(&deviceRef);
        if (ok) {
            if (maxWidth) {
                *maxWidth = width;
            }
            if (maxHeight) {
                *maxHeight = height;
            }
            ALOGD("ffmpeg_hwaccel_limits: %s max %dx%d", codec->name, width, height);
            return 1;
        }
    }

    ALOGD("ffmpeg_hwaccel_limits: %s limits unknown", codec->name);
    return 0;
}
