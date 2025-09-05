/* vaapi_common.h
 *
 * Copyright (c) 2003-2024 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HANDBRAKE_VAAPI_COMMON_H
#define HANDBRAKE_VAAPI_COMMON_H

#include <libavcodec/avcodec.h>

int hb_vaapi_h264_available(void);
int hb_vaapi_h265_available(void);
int hb_vaapi_h265_10bit_available(void);

// Capability query functions
int hb_vaapi_supports_bframes(int vcodec);
int hb_vaapi_get_max_width(int vcodec);
int hb_vaapi_get_max_height(int vcodec);

// Rate control mode query functions
int hb_vaapi_supports_cqp(int vcodec);
int hb_vaapi_supports_vbr(int vcodec);
int hb_vaapi_supports_cbr(int vcodec);
uint32_t hb_vaapi_get_rc_modes(int vcodec);

// Hardware decoder support functions
const char* hb_vaapi_decode_get_codec_name(enum AVCodecID codec_id);
int hb_vaapi_decode_is_codec_supported(int adapter_index, int video_codec_param, int pix_fmt, int width, int height);
int hb_vaapi_decode_h264_is_supported(void);
int hb_vaapi_decode_h265_is_supported(void);
int hb_vaapi_decode_h265_10bit_is_supported(void);
int hb_vaapi_decode_av1_is_supported(void);
int hb_vaapi_available(void);

#endif // HANDBRAKE_VAAPI_COMMON_H