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
    const char *codec_name = hb_vaapi_decode_get_codec_name(codec_param);
    if (codec_name != NULL)
    {
        return (void *)avcodec_find_decoder_by_name(codec_name);
    }
    // Fallback to default decoder if VAAPI decoder not available
    return NULL;
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
    .caps         = 0
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
    // Check if VAAPI is available on the system
    // Cache the result to avoid repeated detection
    if (vaapi_available == -1)
    {
        // VAAPI is available if any codec is supported
        vaapi_available = hb_vaapi_h264_available() ||
                         hb_vaapi_h265_available() ||
                         hb_vaapi_h265_10bit_available();
    }
    return vaapi_available;
}

#else // !HB_PROJECT_FEATURE_VAAPI

// Stub implementations when VAAPI is disabled
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