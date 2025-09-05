/* vaapi_common.h
 *
 * Copyright (c) 2003-2025 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HANDBRAKE_VAAPI_COMMON_H
#define HANDBRAKE_VAAPI_COMMON_H

#include "handbrake/common.h"

// Hardware availability checks
int hb_vaapi_h264_available(void);
int hb_vaapi_h265_available(void);
int hb_vaapi_h265_10bit_available(void);

// Job setup and validation
int hb_vaapi_setup_job(hb_job_t *job);

// Encoder/decoder utilities
const char* hb_vaapi_decode_get_codec_name(int codec);
int hb_vaapi_is_encoder(int vcodec);

// Rate control support
int hb_vaapi_supports_cqp(int vcodec);
int hb_vaapi_supports_vbr(int vcodec);
int hb_vaapi_supports_cbr(int vcodec);
int hb_vaapi_supports_bframes(int vcodec);

// General availability and decode support
int hb_vaapi_available(void);
int hb_vaapi_decode_is_codec_supported(int adapter_index, int codec_id, int pix_fmt, int width, int height);

#endif // HANDBRAKE_VAAPI_COMMON_H