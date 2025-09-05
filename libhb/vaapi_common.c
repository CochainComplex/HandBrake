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
    .find_decoder = NULL, // Use default
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
                
                vaTerminate(va_dpy);
                close(fd);
                
                if (has_profile)
                {
                    hb_log("VAAPI: %s profile supported on %s (VA-API %d.%d)\n", profile_name, render_nodes[i], major, minor);
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

#endif // HB_PROJECT_FEATURE_VAAPI