/* vaapi_common.c
 *
 * Copyright (c) 2003-2025 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/vaapi_common.h"

#if HB_PROJECT_FEATURE_VAAPI

#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <libdrm/drm.h>
#include <xf86drm.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavcodec/avcodec.h>

// Hardware capability cache - simple and effective
static int g_vaapi_init_done = 0;
static int g_h264_available = -1;
static int g_h265_available = -1;
static int g_h265_10bit_available = -1;

// Supported encoder list
static const int vaapi_encoders[] = {
    HB_VCODEC_FFMPEG_VAAPI_H264,
    HB_VCODEC_FFMPEG_VAAPI_H265,
    HB_VCODEC_FFMPEG_VAAPI_H265_10BIT,
    HB_VCODEC_INVALID
};

// Forward declarations
static int check_profile_support(VAProfile profile);
static int vaapi_device_is_usable(VADisplay dpy);
static void vaapi_init_capabilities(void);
const char* hb_vaapi_decode_get_codec_name(int codec);

// Simple filter check - VAAPI supports basic filters and scale_vaapi
static int vaapi_are_filters_supported(hb_list_t *filters)
{
    // VAAPI can handle basic filters
    // Complex filters may need CPU fallback
    return 1;
}

// Find VAAPI decoder for codec
static void * vaapi_find_decoder(int codec_param)
{
    const char *codec_name = hb_vaapi_decode_get_codec_name(codec_param);
    if (codec_name)
    {
        return (void *)avcodec_find_decoder_by_name(codec_name);
    }
    return NULL;
}

// Hardware accelerator registration structure
hb_hwaccel_t hb_hwaccel_vaapi = {
    .id           = HB_DECODE_VAAPI,
    .name         = "vaapi",
    .encoders     = vaapi_encoders,
    .type         = AV_HWDEVICE_TYPE_VAAPI,
    .hw_pix_fmt   = AV_PIX_FMT_VAAPI,
    .can_filter   = vaapi_are_filters_supported,
    .find_decoder = vaapi_find_decoder,
    .upload       = NULL, // Use default
    .caps         = HB_HWACCEL_CAP_SCAN  // Support hardware scanning
};

// Initialize VAAPI capabilities once
static void vaapi_init_capabilities(void)
{
    if (g_vaapi_init_done)
        return;
    
    g_vaapi_init_done = 1;
    
    // Check for codec support
    g_h264_available = check_profile_support(VAProfileH264Main) || 
                      check_profile_support(VAProfileH264High);
    
    g_h265_available = check_profile_support(VAProfileHEVCMain);
    g_h265_10bit_available = check_profile_support(VAProfileHEVCMain10);
    
    hb_log("VAAPI: Initialization complete - H.264:%d H.265:%d H.265-10bit:%d",
           g_h264_available, g_h265_available, g_h265_10bit_available);
}

// Check if a specific VA profile is supported (KISS approach from old implementation)
static int check_profile_support(VAProfile profile)
{
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
            continue;
        
        // Check for supported GPU drivers
        drmVersion *version = drmGetVersion(fd);
        if (version)
        {
            int is_supported = (strcmp(version->name, "amdgpu") == 0 ||
                               strcmp(version->name, "radeon") == 0 ||
                               strcmp(version->name, "i915") == 0);
            drmFreeVersion(version);
            
            if (!is_supported)
            {
                close(fd);
                continue;
            }
        }
        
        // Initialize VA-API
        VADisplay dpy = vaGetDisplayDRM(fd);
        if (!dpy)
        {
            close(fd);
            continue;
        }
        
        int major, minor;
        if (vaInitialize(dpy, &major, &minor) == VA_STATUS_SUCCESS)
        {
            // Check if device is actually usable
            if (!vaapi_device_is_usable(dpy))
            {
                vaTerminate(dpy);
                close(fd);
                continue;
            }
            
            // Query profiles
            VAProfile profiles[32];
            int num_profiles = 0;
            vaQueryConfigProfiles(dpy, profiles, &num_profiles);
            
            for (int j = 0; j < num_profiles; j++)
            {
                if (profiles[j] == profile)
                {
                    vaTerminate(dpy);
                    close(fd);
                    return 1; // Profile supported
                }
            }
            
            vaTerminate(dpy);
        }
        close(fd);
    }
    
    return 0; // Profile not supported
}

// Check if VAAPI device is actually usable (can create config)
static int vaapi_device_is_usable(VADisplay dpy)
{
    // Try to create a simple H264 config to verify device works
    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    
    VAStatus status = vaGetConfigAttributes(dpy, VAProfileH264Main,
                                           VAEntrypointEncSlice, &attrib, 1);
    
    if (status != VA_STATUS_SUCCESS)
    {
        // Try with decode to see if device works at all
        status = vaGetConfigAttributes(dpy, VAProfileH264Main,
                                      VAEntrypointVLD, &attrib, 1);
    }
    
    return (status == VA_STATUS_SUCCESS);
}

// Public API: Check H.264 support
int hb_vaapi_h264_available(void)
{
    if (hb_is_hardware_disabled())
        return 0;
    
    vaapi_init_capabilities();
    return g_h264_available;
}

// Public API: Check H.265 support  
int hb_vaapi_h265_available(void)
{
    if (hb_is_hardware_disabled())
        return 0;
    
    vaapi_init_capabilities();
    return g_h265_available;
}

// Public API: Check H.265 10-bit support
int hb_vaapi_h265_10bit_available(void)
{
    if (hb_is_hardware_disabled())
        return 0;
    
    vaapi_init_capabilities();
    return g_h265_10bit_available;
}

// Get decoder name for codec
const char* hb_vaapi_decode_get_codec_name(int codec)
{
    switch (codec)
    {
        case AV_CODEC_ID_H264:
            return "h264_vaapi";
        case AV_CODEC_ID_HEVC:
            return "hevc_vaapi";
        case AV_CODEC_ID_AV1:
            return "av1_vaapi";
        case AV_CODEC_ID_VP9:
            return "vp9_vaapi";
        case AV_CODEC_ID_VP8:
            return "vp8_vaapi";
        case AV_CODEC_ID_MPEG2VIDEO:
            return "mpeg2_vaapi";
        default:
            return NULL;
    }
}

// Check if codec is VAAPI encoder
int hb_vaapi_is_encoder(int vcodec)
{
    return (vcodec == HB_VCODEC_FFMPEG_VAAPI_H264 ||
            vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 ||
            vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT);
}

// Validate and setup VAAPI job
int hb_vaapi_setup_job(hb_job_t *job)
{
    if (!job)
        return -1;
    
    // Initialize capabilities if needed
    vaapi_init_capabilities();
    
    // Check encoder support
    if (hb_vaapi_is_encoder(job->vcodec))
    {
        if (job->vcodec == HB_VCODEC_FFMPEG_VAAPI_H264 && !g_h264_available)
        {
            hb_log("VAAPI: H.264 encoder not available");
            return -1;
        }
        if (job->vcodec == HB_VCODEC_FFMPEG_VAAPI_H265 && !g_h265_available)
        {
            hb_log("VAAPI: H.265 encoder not available");
            return -1;
        }
        if (job->vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT && !g_h265_10bit_available)
        {
            hb_log("VAAPI: H.265 10-bit encoder not available");
            return -1;
        }
    }
    
    // Validate resolution limits (conservative defaults)
    if (job->width > 4096 || job->height > 4096)
    {
        hb_log("VAAPI: Resolution %dx%d exceeds hardware limits", job->width, job->height);
        return -1;
    }
    
    return 0;
}

// Rate control support checks (simplified)
int hb_vaapi_supports_cqp(int vcodec)
{
    // Most VAAPI hardware supports CQP
    return hb_vaapi_is_encoder(vcodec);
}

int hb_vaapi_supports_vbr(int vcodec)
{
    // VBR is widely supported
    return hb_vaapi_is_encoder(vcodec);
}

int hb_vaapi_supports_cbr(int vcodec)
{
    // CBR support varies by hardware
    return hb_vaapi_is_encoder(vcodec);
}

int hb_vaapi_supports_bframes(int vcodec)
{
    // Conservative: disable B-frames by default
    // Some hardware supports it but can cause issues
    return 0;
}

#endif // HB_PROJECT_FEATURE_VAAPI