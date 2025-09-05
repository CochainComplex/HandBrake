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

int hb_vaapi_h264_available(void);
int hb_vaapi_h265_available(void);
int hb_vaapi_h265_10bit_available(void);

// Capability query functions
int hb_vaapi_supports_bframes(int vcodec);
int hb_vaapi_get_max_width(int vcodec);
int hb_vaapi_get_max_height(int vcodec);

#endif // HANDBRAKE_VAAPI_COMMON_H