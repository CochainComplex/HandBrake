/* vaapi_common.c
 *
 * Copyright (c) 2003-2024 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/vaapi_common.h"

#if HB_PROJECT_FEATURE_VAAPI

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <libdrm/drm.h>
#include <xf86drm.h>

// FFmpeg headers - will be available after contrib build
#ifdef __has_include
#if __has_include(<libavutil/hwcontext.h>)
#include <libavutil/hwcontext.h>
#define HAS_FFMPEG_HWCONTEXT 1
#endif
#else
// Fallback for compilers without __has_include
#include <libavutil/hwcontext.h>
#define HAS_FFMPEG_HWCONTEXT 1
#endif

static int is_h264_available = -1;
static int is_h265_available = -1;
static int vaapi_available = -1;  // Overall VAAPI availability

// VAAPI capability cache
typedef struct {
    int supports_bframes;     // -1 = unknown, 0 = no, 1 = yes
    int supports_10bit;       // -1 = unknown, 0 = no, 1 = yes
    int max_width;
    int max_height;
    uint32_t rate_control_modes;  // Bitmask of supported RC modes
    int quality_levels;       // Number of quality levels supported
    uint32_t packed_headers;  // Bitmask of supported packed headers
} hb_vaapi_caps_t;

static hb_vaapi_caps_t h264_caps = {-1, -1, 0, 0, 0, 0, 0};
static hb_vaapi_caps_t h265_caps = {-1, -1, 0, 0, 0, 0, 0};

// Forward declaration for capability querying function
static void query_vaapi_capabilities(VADisplay va_dpy, VAProfile profile, hb_vaapi_caps_t *caps);

// Encoder list for hwaccel structure
static const int vaapi_encoders[] = {
    HB_VCODEC_FFMPEG_VAAPI_H264,
    HB_VCODEC_FFMPEG_VAAPI_H265,
    HB_VCODEC_FFMPEG_VAAPI_H265_10BIT,
    HB_VCODEC_INVALID
};

// Helper function to check if a codec is a VAAPI encoder (DRY principle)
int hb_vaapi_is_encoder(int vcodec)
{
    return (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264 ||
            vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 ||
            vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT);
}

// Simple filter compatibility check
static int vaapi_are_filters_supported(hb_list_t *filters)
{
    // VAAPI supports basic filters
    // More complex filters may require frame download/upload
    return 1;
}

// Function to find VAAPI hardware decoder for a given codec
static void * vaapi_find_decoder(int codec_param)
{
    hb_log("VAAPI: vaapi_find_decoder called with codec_param: %d (0x%x)", codec_param, codec_param);
    
    // First check if VAAPI hardware is actually available (KISS - fail fast)
    if (!hb_vaapi_available()) 
    {
        hb_log("VAAPI: Hardware not available, skipping VAAPI decoder lookup");
        // Return software decoder as fallback instead of NULL
        const AVCodec *sw_codec = avcodec_find_decoder(codec_param);
        if (sw_codec)
        {
            hb_log("VAAPI: Returning software decoder %s as fallback", sw_codec->name);
        }
        return (void *)sw_codec;
    }
    
    const char *codec_name = hb_vaapi_decode_get_codec_name(codec_param);
    if (codec_name != NULL)
    {
        hb_log("VAAPI: Looking for hardware decoder: %s", codec_name);
        
        // Validate codec is actually supported by hardware before lookup
        if (!hb_vaapi_decode_is_codec_supported(0, codec_param, AV_PIX_FMT_NV12, 1920, 1080))
        {
            hb_log("VAAPI: Codec %d not supported by hardware, falling back to software", codec_param);
            const AVCodec *sw_codec = avcodec_find_decoder(codec_param);
            if (sw_codec)
            {
                hb_log("VAAPI: Using software decoder %s", sw_codec->name);
            }
            return (void *)sw_codec;
        }
        
        const AVCodec *codec = avcodec_find_decoder_by_name(codec_name);
        if (codec != NULL)
        {
            hb_log("VAAPI: Successfully found hardware decoder: %s (codec id: %d)", codec_name, codec->id);
            return (void *)codec;
        }
        else
        {
            hb_log("VAAPI: ERROR - Hardware decoder %s not found in FFmpeg build", codec_name);
            hb_log("VAAPI: This usually means FFmpeg was built without VAAPI decoder support");
        }
    }
    else
    {
        hb_log("VAAPI: No VAAPI decoder name mapping for codec_param: %d", codec_param);
    }
    
    // Always return software decoder as fallback instead of NULL (prevents scan failure)
    hb_log("VAAPI: Hardware decoder lookup failed, falling back to software decoding");
    const AVCodec *sw_codec = avcodec_find_decoder(codec_param);
    if (sw_codec)
    {
        hb_log("VAAPI: Using software decoder %s as fallback", sw_codec->name);
    }
    return (void *)sw_codec;
}

// Hardware accelerator structure
hb_hwaccel_t hb_hwaccel_vaapi =
{
    .id           = HB_DECODE_VAAPI,
    .name         = "vaapi",
    .encoders     = vaapi_encoders,
#ifdef HAS_FFMPEG_HWCONTEXT
    .type         = AV_HWDEVICE_TYPE_VAAPI,
    .hw_pix_fmt   = AV_PIX_FMT_VAAPI,
#else
    // These will be set properly once FFmpeg headers are available
    .type         = -1,
    .hw_pix_fmt   = -1,
#endif
    .can_filter   = vaapi_are_filters_supported,
    .find_decoder = vaapi_find_decoder,  // Use VAAPI-specific decoder lookup
    .upload       = NULL, // Use default
    .caps         = HB_HWACCEL_CAP_SCAN
};

static int check_vaapi_codec_support(VAProfile profile_to_check)
{
    const char* profile_name = 
        (profile_to_check == VAProfileH264Main) ? "H264 Main" :
        (profile_to_check == VAProfileH264High) ? "H264 High" :
        (profile_to_check == VAProfileHEVCMain) ? "HEVC Main" :
        (profile_to_check == VAProfileHEVCMain10) ? "HEVC Main10" : "Unknown";
    
    hb_log("VAAPI: Checking for %s profile support\n", profile_name);
    
    if (hb_is_hardware_disabled())
    {
        hb_log("VAAPI: Hardware encoding disabled\n");
        return 0;
    }

    // Try to open render nodes
    const char *render_nodes[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        "/dev/dri/card1",
        NULL
    };

    for (int i = 0; render_nodes[i] != NULL; i++)
    {
        int fd = open(render_nodes[i], O_RDWR);
        if (fd < 0)
        {
            hb_log("VAAPI: Cannot open %s\n", render_nodes[i]);
            continue;
        }

        // Check GPU driver (AMD, Intel, etc.)
        drmVersion *version = drmGetVersion(fd);
        if (version)
        {
            hb_log("VAAPI: Found DRM driver: %s on %s\n", version->name, render_nodes[i]);
            
            // Support AMD (amdgpu/radeon) and Intel (i915) GPUs
            int is_supported = (strcmp(version->name, "amdgpu") == 0 || 
                               strcmp(version->name, "radeon") == 0 ||
                               strcmp(version->name, "i915") == 0);
            drmFreeVersion(version);
            
            if (!is_supported)
            {
                hb_log("VAAPI: Unsupported driver, skipping\n");
                close(fd);
                continue;
            }
        }
        else
        {
            hb_log("VAAPI: Cannot get DRM version for %s\n", render_nodes[i]);
            close(fd);
            continue;
        }

        // Try to initialize VAAPI
        VADisplay va_dpy = vaGetDisplayDRM(fd);
        if (va_dpy)
        {
            int major, minor;
            VAStatus status = vaInitialize(va_dpy, &major, &minor);
            if (status == VA_STATUS_SUCCESS)
            {
                // Check for specific codec support
                VAProfile profiles[32];
                int num_profiles = 0;
                vaQueryConfigProfiles(va_dpy, profiles, &num_profiles);
                
                int has_profile = 0;
                for (int j = 0; j < num_profiles; j++)
                {
                    if (profiles[j] == profile_to_check)
                    {
                        has_profile = 1;
                        break;
                    }
                }
                
                if (has_profile)
                {
                    hb_log("VAAPI: %s profile supported on %s (VA-API %d.%d)\n", profile_name, render_nodes[i], major, minor);
                    
                    // Query capabilities for this profile BEFORE terminating VA display
                    hb_vaapi_caps_t *caps_ptr = NULL;
                    if (profile_to_check == VAProfileH264Main || profile_to_check == VAProfileH264High)
                    {
                        caps_ptr = &h264_caps;
                    }
                    else if (profile_to_check == VAProfileHEVCMain || profile_to_check == VAProfileHEVCMain10)
                    {
                        caps_ptr = &h265_caps;
                    }
                    
                    if (caps_ptr && caps_ptr->max_width == 0) // Only query once
                    {
                        query_vaapi_capabilities(va_dpy, profile_to_check, caps_ptr);
                    }
                }
                
                vaTerminate(va_dpy);
                close(fd);
                
                if (has_profile)
                {
                    return 1;
                }
                else
                {
                    hb_log("VAAPI: %s profile not supported on %s (found %d profiles)\n", profile_name, render_nodes[i], num_profiles);
                }
            }
            else
            {
                const char* error_msg = vaErrorStr(status);
                hb_log("VAAPI: Failed to initialize VA-API on %s: %s (status=%d)\n", render_nodes[i], error_msg, status);
                close(fd);
            }
        }
        else
        {
            close(fd);
        }
    }
    
    hb_log("VAAPI: No suitable hardware encoder found for %s profile\n", profile_name);
    return 0;
}

// Query VAAPI encoder capabilities using vendor-agnostic VA attributes
static void query_vaapi_capabilities(VADisplay va_dpy, VAProfile profile, hb_vaapi_caps_t *caps)
{
    if (!caps || !va_dpy) return;
    
    // Check if encoder entrypoint is available
    VAEntrypoint entrypoints[10];
    int num_entrypoints = 0;
    VAStatus status = vaQueryConfigEntrypoints(va_dpy, profile, entrypoints, &num_entrypoints);
    
    if (status != VA_STATUS_SUCCESS || num_entrypoints == 0) {
        return;
    }
    
    // Look for encode entrypoint
    int has_encode = 0;
    for (int i = 0; i < num_entrypoints; i++) {
        if (entrypoints[i] == VAEntrypointEncSlice || 
            entrypoints[i] == VAEntrypointEncSliceLP) {
            has_encode = 1;
            break;
        }
    }
    
    if (!has_encode) return;
    
    // Query configuration attributes
    VAConfigAttrib attrs[7];
    attrs[0].type = VAConfigAttribRateControl;
    attrs[1].type = VAConfigAttribMaxPictureWidth;
    attrs[2].type = VAConfigAttribMaxPictureHeight;
    attrs[3].type = VAConfigAttribRTFormat;
    attrs[4].type = VAConfigAttribEncMaxRefFrames;
    attrs[5].type = VAConfigAttribEncQualityRange;
    attrs[6].type = VAConfigAttribEncPackedHeaders;
    
    status = vaGetConfigAttributes(va_dpy, profile, VAEntrypointEncSlice, attrs, 7);
    if (status == VA_STATUS_SUCCESS) {
        // Rate control modes
        if (attrs[0].value != VA_ATTRIB_NOT_SUPPORTED) {
            caps->rate_control_modes = attrs[0].value;
        }
        
        // Max resolution
        if (attrs[1].value != VA_ATTRIB_NOT_SUPPORTED) {
            caps->max_width = attrs[1].value;
        }
        if (attrs[2].value != VA_ATTRIB_NOT_SUPPORTED) {
            caps->max_height = attrs[2].value;
        }
        
        // RT formats (pixel formats) - check for 10-bit support
        if (attrs[3].value != VA_ATTRIB_NOT_SUPPORTED) {
            // Check if 10-bit formats are supported
            if (attrs[3].value & VA_RT_FORMAT_YUV420_10) {
                caps->supports_10bit = 1;
            } else {
                caps->supports_10bit = 0;
            }
        }
        
        // Reference frames - can indicate B-frame support
        if (attrs[4].value != VA_ATTRIB_NOT_SUPPORTED && attrs[4].value > 1) {
            // If more than 1 reference frame, likely supports B-frames
            // This is a heuristic - more sophisticated detection could be added
            caps->supports_bframes = (attrs[4].value > 2) ? 1 : 0;
        }
        
        // Quality range - number of quality levels
        if (attrs[5].value != VA_ATTRIB_NOT_SUPPORTED) {
            caps->quality_levels = attrs[5].value;
        }
        
        // Packed headers support
        if (attrs[6].value != VA_ATTRIB_NOT_SUPPORTED) {
            caps->packed_headers = attrs[6].value;
        }
        
        hb_log("VAAPI: Capabilities - RC modes: 0x%x, Max res: %dx%d, 10bit: %d, B-frames: %d, Quality levels: %d\n",
               caps->rate_control_modes, caps->max_width, caps->max_height, 
               caps->supports_10bit, caps->supports_bframes, caps->quality_levels);
    }
}

int hb_vaapi_h264_available(void)
{
    if (is_h264_available == -1)
    {
        hb_log("VAAPI: Checking H.264 encoder availability\n");
        // Check for H.264 Main or High profile
        is_h264_available = check_vaapi_codec_support(VAProfileH264Main) ||
                            check_vaapi_codec_support(VAProfileH264High);
        hb_log("VAAPI: H.264 encoder %s\n", is_h264_available ? "available" : "not available");
    }
    return is_h264_available;
}

int hb_vaapi_h265_available(void)
{
    if (is_h265_available == -1)
    {
        hb_log("VAAPI: Checking H.265 encoder availability\n");
        // Check for HEVC Main profile
        is_h265_available = check_vaapi_codec_support(VAProfileHEVCMain);
        hb_log("VAAPI: H.265 encoder %s\n", is_h265_available ? "available" : "not available");
    }
    return is_h265_available;
}

int hb_vaapi_h265_10bit_available(void)
{
    // Check for HEVC Main10 profile
    static int is_h265_10bit_available = -1;
    if (is_h265_10bit_available == -1)
    {
        hb_log("VAAPI: Checking H.265 10-bit encoder availability\n");
        is_h265_10bit_available = check_vaapi_codec_support(VAProfileHEVCMain10);
        hb_log("VAAPI: H.265 10-bit encoder %s\n", is_h265_10bit_available ? "available" : "not available");
    }
    return is_h265_10bit_available;
}

int hb_vaapi_supports_bframes(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        // Ensure capabilities are queried
        hb_vaapi_h264_available();
        return h264_caps.supports_bframes > 0 ? 1 : 0;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return h265_caps.supports_bframes > 0 ? 1 : 0;
    }
    return 0;
}

int hb_vaapi_get_max_width(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return h264_caps.max_width > 0 ? h264_caps.max_width : 4096; // Default to 4K
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return h265_caps.max_width > 0 ? h265_caps.max_width : 8192; // Default to 8K
    }
    return 4096;
}

int hb_vaapi_get_max_height(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return h264_caps.max_height > 0 ? h264_caps.max_height : 4096;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return h265_caps.max_height > 0 ? h265_caps.max_height : 8192;
    }
    return 4096;
}

// Define VA-API rate control mode bits if not already defined
#ifndef VA_RC_CQP
#define VA_RC_CQP 0x00000010
#endif
#ifndef VA_RC_CBR
#define VA_RC_CBR 0x00000002
#endif
#ifndef VA_RC_VBR
#define VA_RC_VBR 0x00000004
#endif

int hb_vaapi_supports_cqp(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return (h264_caps.rate_control_modes & VA_RC_CQP) ? 1 : 0;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return (h265_caps.rate_control_modes & VA_RC_CQP) ? 1 : 0;
    }
    return 0;
}

int hb_vaapi_supports_vbr(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return (h264_caps.rate_control_modes & VA_RC_VBR) ? 1 : 0;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return (h265_caps.rate_control_modes & VA_RC_VBR) ? 1 : 0;
    }
    return 0;
}

int hb_vaapi_supports_cbr(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return (h264_caps.rate_control_modes & VA_RC_CBR) ? 1 : 0;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return (h265_caps.rate_control_modes & VA_RC_CBR) ? 1 : 0;
    }
    return 0;
}

uint32_t hb_vaapi_get_rc_modes(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264)
    {
        hb_vaapi_h264_available();
        return h264_caps.rate_control_modes;
    }
    else if (vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 || 
             vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT)
    {
        hb_vaapi_h265_available();
        return h265_caps.rate_control_modes;
    }
    return 0;
}

// VAAPI Hardware Decoder Support Functions
// Following the QSV decoder pattern for consistency

const char* hb_vaapi_decode_get_codec_name(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return "h264_vaapi";
        case AV_CODEC_ID_HEVC:
            return "hevc_vaapi";
        case AV_CODEC_ID_AV1:
            return "av1_vaapi";
        default:
            return NULL;
    }
}

int hb_vaapi_decode_h264_is_supported(void)
{
    // H264 decoder is supported if encoder is supported
    // They use the same hardware capabilities
    return hb_vaapi_h264_available();
}

int hb_vaapi_decode_h265_is_supported(void)
{
    // H265 decoder is supported if encoder is supported
    return hb_vaapi_h265_available();
}

int hb_vaapi_decode_h265_10bit_is_supported(void)
{
    // H265 10-bit decoder is supported if encoder is supported
    return hb_vaapi_h265_10bit_available();
}

int hb_vaapi_decode_av1_is_supported(void)
{
    // Check if AV1 decoding is supported
    // For now, we'll check if VAAPI is available
    // Future: Add specific AV1 capability detection
    return hb_vaapi_available();
}

int hb_vaapi_decode_is_codec_supported(int adapter_index, int video_codec_param, int pix_fmt, int width, int height)
{
    // adapter_index is not used for VAAPI (unlike QSV)
    // Check if the codec and pixel format combination is supported
    
    switch (video_codec_param)
    {
        case AV_CODEC_ID_H264:
            if (pix_fmt == AV_PIX_FMT_NV12 || 
                pix_fmt == AV_PIX_FMT_YUV420P || 
                pix_fmt == AV_PIX_FMT_YUVJ420P)
            {
                return hb_vaapi_decode_h264_is_supported();
            }
            break;
            
        case AV_CODEC_ID_HEVC:
            if (pix_fmt == AV_PIX_FMT_NV12 || 
                pix_fmt == AV_PIX_FMT_YUV420P || 
                pix_fmt == AV_PIX_FMT_YUVJ420P)
            {
                return hb_vaapi_decode_h265_is_supported();
            }
            else if (pix_fmt == AV_PIX_FMT_P010LE || 
                     pix_fmt == AV_PIX_FMT_YUV420P10)
            {
                return hb_vaapi_decode_h265_10bit_is_supported();
            }
            break;
            
        case AV_CODEC_ID_AV1:
            if (pix_fmt == AV_PIX_FMT_NV12 || 
                pix_fmt == AV_PIX_FMT_YUV420P)
            {
                return hb_vaapi_decode_av1_is_supported();
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

int hb_vaapi_available(void)
{
    // Singleton pattern with comprehensive validation (DRY - single detection)
    // Cache the result to avoid repeated detection
    if (vaapi_available == -1)
    {
        hb_log("VAAPI: Checking hardware availability...");
        
        // First check if hardware is globally disabled
        if (hb_is_hardware_disabled())
        {
            hb_log("VAAPI: Hardware globally disabled");
            vaapi_available = 0;
            return vaapi_available;
        }
        
        // Check individual codec encoding support
        int h264_avail = hb_vaapi_h264_available();
        int h265_avail = hb_vaapi_h265_available();
        int h265_10bit_avail = hb_vaapi_h265_10bit_available();
        
        hb_log("VAAPI: H.264 encoder available: %d", h264_avail);
        hb_log("VAAPI: H.265 encoder available: %d", h265_avail);
        hb_log("VAAPI: H.265 10-bit encoder available: %d", h265_10bit_avail);
        
        vaapi_available = h264_avail || h265_avail || h265_10bit_avail;
        
        // Additional validation: ensure FFmpeg VAAPI decoders are available
        if (vaapi_available)
        {
            int decoder_count = 0;
            const char* test_codecs[] = {"h264_vaapi", "hevc_vaapi", "av1_vaapi", NULL};
            
            hb_log("VAAPI: Validating FFmpeg decoder availability...");
            for (int i = 0; test_codecs[i] != NULL; i++)
            {
                const AVCodec *codec = avcodec_find_decoder_by_name(test_codecs[i]);
                if (codec != NULL)
                {
                    decoder_count++;
                    hb_log("VAAPI: FFmpeg decoder %s found", test_codecs[i]);
                }
                else
                {
                    hb_log("VAAPI: FFmpeg decoder %s not available", test_codecs[i]);
                }
            }
            
            if (decoder_count == 0)
            {
                hb_log("VAAPI: WARNING - No FFmpeg VAAPI decoders found, hardware decoding disabled");
                hb_log("VAAPI: Encoders may still work but will require software decode");
                // Don't disable completely - encoders can still work with software decode
            }
            else
            {
                hb_log("VAAPI: Found %d FFmpeg VAAPI decoder(s)", decoder_count);
            }
        }
        
        hb_log("VAAPI: Overall availability: %d (encoders: %s, decoders: checked)", 
               vaapi_available, vaapi_available ? "yes" : "no");
    }
    return vaapi_available;
}

// Centralized VAAPI job setup - follows QSV pattern (KISS/SOLID)
int hb_vaapi_setup_job(hb_job_t *job)
{
    // Always return success like QSV - validation modifies job flags
    if (!job)
    {
        return 0;
    }
    
    // Check if VAAPI is available at all
    if (!hb_vaapi_available())
    {
        // Clear any VAAPI flags if hardware not available
        if (job->hw_decode & HB_DECODE_VAAPI)
        {
            job->hw_decode &= ~HB_DECODE_VAAPI;
        }
        if (hb_vaapi_is_encoder(job->vcodec))
        {
            hb_log("VAAPI: Hardware not available, falling back to software");
            // TODO: Set software encoder fallback
        }
        return 0;
    }
    
    // Validate decoder support if requested
    if (job->hw_decode & HB_DECODE_VAAPI)
    {
        // Check if codec is supported for decoding
        int is_supported = 0;
        if (job->title && job->title->video_codec_param)
        {
            is_supported = hb_vaapi_decode_is_codec_supported(
                0,  // adapter_index - currently ignored
                job->title->video_codec_param,
                job->input_pix_fmt,
                job->title->geometry.width,
                job->title->geometry.height
            );
        }
        
        if (!is_supported)
        {
            // Silent fallback - remove VAAPI decode flag
            job->hw_decode &= ~HB_DECODE_VAAPI;
        }
    }
    
    // Validate encoder support if VAAPI encoder selected
    if (hb_vaapi_is_encoder(job->vcodec))
    {
        int encoder_supported = 0;
        
        // Check codec-specific support
        switch (job->vcodec)
        {
            case HB_VCODEC_FFMPEG_VAAPI_H264:
                encoder_supported = hb_vaapi_h264_available();
                break;
            case HB_VCODEC_FFMPEG_VAAPI_H265:
                encoder_supported = hb_vaapi_h265_available();
                break;
            case HB_VCODEC_FFMPEG_VAAPI_H265_10BIT:
                encoder_supported = hb_vaapi_h265_10bit_available();
                break;
        }
        
        // Check resolution limits
        if (encoder_supported)
        {
            int max_width = hb_vaapi_get_max_width(job->vcodec);
            int max_height = hb_vaapi_get_max_height(job->vcodec);
            
            if ((max_width > 0 && job->width > max_width) ||
                (max_height > 0 && job->height > max_height))
            {
                hb_log("VAAPI: Resolution %dx%d exceeds hardware limits %dx%d",
                       job->width, job->height, max_width, max_height);
                encoder_supported = 0;
            }
        }
        
        // Validate rate control mode compatibility
        if (encoder_supported)
        {
            if (job->vquality >= 0) // Quality-based encoding
            {
                if (!hb_vaapi_supports_cqp(job->vcodec))
                {
                    // Try VBR fallback
                    if (!hb_vaapi_supports_vbr(job->vcodec))
                    {
                        hb_log("VAAPI: No suitable rate control mode for quality encoding");
                        encoder_supported = 0;
                    }
                }
            }
            else // Bitrate-based encoding
            {
                if (!hb_vaapi_supports_vbr(job->vcodec) && 
                    !hb_vaapi_supports_cbr(job->vcodec))
                {
                    hb_log("VAAPI: No suitable rate control mode for bitrate encoding");
                    encoder_supported = 0;
                }
            }
        }
        
        if (!encoder_supported)
        {
            hb_log("VAAPI: Encoder not supported for current configuration");
            // TODO: Set software encoder fallback
        }
        
        // Cache capabilities for encoder use (avoids repeated queries)
        // This is done automatically by the availability check functions
    }
    
    return 0;  // Always return success like QSV
}

#else // !HB_PROJECT_FEATURE_VAAPI

// Stub implementations when VAAPI is disabled
int hb_vaapi_is_encoder(int vcodec)
{
    return 0;
}

int hb_vaapi_setup_job(hb_job_t *job)
{
    return 0;
}

int hb_vaapi_h264_available(void)
{
    return 0;
}

int hb_vaapi_h265_available(void)
{
    return 0;
}

int hb_vaapi_h265_10bit_available(void)
{
    return 0;
}

int hb_vaapi_supports_bframes(int vcodec)
{
    return 0;
}

int hb_vaapi_get_max_width(int vcodec)
{
    return 4096;
}

int hb_vaapi_get_max_height(int vcodec)
{
    return 4096;
}

int hb_vaapi_supports_cqp(int vcodec)
{
    return 0;
}

int hb_vaapi_supports_vbr(int vcodec)
{
    return 0;
}

int hb_vaapi_supports_cbr(int vcodec)
{
    return 0;
}

uint32_t hb_vaapi_get_rc_modes(int vcodec)
{
    return 0;
}

#endif // HB_PROJECT_FEATURE_VAAPI