/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

G_BEGIN_DECLS

#include "common/image.h"
#include "common/mipmap_cache.h"

dt_imageio_retval_t dt_imageio_open_ciraw(dt_image_t *img, const char *filename,
                                          dt_mipmap_buffer_t *buf);

// spawn the background kernel warmup (no-op on systems without CIRAW
// support): decodes a few library images at thumbnail scale so the
// first interactive decode does not pay the one-time Metal build
void dt_ciraw_warmup_start(void);

// mark the calling thread as a background decoder: its ciraw decodes go
// through a separate single-slot gate so they never starve the
// interactive path (call again with FALSE before the thread exits)
void dt_ciraw_decode_set_background(gboolean background);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
