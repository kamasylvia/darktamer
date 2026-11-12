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
/*
 * cmocka unit tests for the framing (borders) blur leg under a zoomed
 * viewport: roi_in is a window of the picture there, so any geometry still
 * sized from roi_in shifts the round-rect clip and lets the backdrop bleed
 * through the picture.
 *
 * The module is #included directly for static access, following
 * test_filmicrgb.c; lib_darktable supplies what it links against.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <cmocka.h>

#include "iop/borders.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// a viewport-sized run must paint the whole picture window opaque: with the
// round-rect center sized from the roi_in window instead of the whole
// picture, the clip rect shifts by (pic - win)/2 and a strip of the picture
// keeps the backdrop color
static void _test_zoomed_viewport_covers_picture(void **state)
{
  // whole picture 200x100; the viewport shows its right half at scale 1,
  // laid out by modify_roi_in the way the pixelpipe does
  dt_dev_pixelpipe_t pipe = { 0 };
  memset(&pipe.image, 0, sizeof(pipe.image));
  dt_dev_pixelpipe_iop_t piece = { 0 };
  piece.pipe = &pipe;
  piece.buf_in = (dt_iop_roi_t){ .x = 0, .y = 0, .width = 200, .height = 100, .scale = 1.0f };

  dt_iop_borders_data_t d = { 0 };
  d.mode = DT_IOP_BORDERS_MODE_BLUR;
  d.backdrop = DT_IOP_BORDERS_BACKDROP_SOLID; // flat red: any clip miss shows red
  d.main_image_width_rate = 90.0f;
  d.mini_top_bottom_margin = 10.0f;
  d.corner_radius = 2.1f;
  d.shadow = 0.0f; // the shadow plane is a separate geometry; keep this test on the clip
  d.text_show = FALSE;
  d.color[0] = 0.8f; d.color[1] = 0.1f; d.color[2] = 0.1f;
  piece.data = &d;

  _yiyin_layout_t L;
  _yiyin_layout(&L, piece.buf_in.width, piece.buf_in.height, &d, NULL, NULL, 0);

  // the viewport: the canvas's right half, so roi_in is the picture's right
  // window and any window-sized geometry shifts the clip left
  const int vw = L.canvas_w / 2;
  dt_iop_roi_t roi_out = { .x = L.canvas_w - vw, .y = 0,
                           .width = vw, .height = L.canvas_h, .scale = 1.0f };
  dt_iop_roi_t roi_in = { 0 };
  modify_roi_in(NULL, &piece, &roi_out, &roi_in);
  assert_true(roi_in.width > 0 && roi_in.width < piece.buf_in.width);

  float *const in = dt_alloc_align_float((size_t)roi_in.width * roi_in.height * 4);
  float *const out = dt_alloc_align_float((size_t)roi_out.width * roi_out.height * 4);
  assert_non_null(in);
  assert_non_null(out);
  // the picture is flat green; the proxy blur of a flat field is the field
  for(size_t k = 0; k < (size_t)roi_in.width * roi_in.height; k++)
  {
    in[k * 4 + 0] = 0.1f;
    in[k * 4 + 1] = 0.8f;
    in[k * 4 + 2] = 0.1f;
    in[k * 4 + 3] = 1.0f;
  }

  _yiyin_render_blur(NULL, &piece, in, out, &roi_in, &roi_out, &d);

  // every plane pixel inside the picture rect (away from the antialiased
  // rim) must be the picture color, not the solid backdrop
  const int pic_x = (int)roundf((float)L.main_left) - roi_out.x;
  const int pic_y = (int)roundf((float)L.main_top) - roi_out.y;
  const int radius = (int)ceilf((float)L.radius) + 2;
  int checked = 0;
  for(int j = 0; j < roi_out.height; j++)
    for(int i = 0; i < roi_out.width; i++)
    {
      const int su = i - pic_x, sv = j - pic_y;
      if(su < radius || sv < radius
         || su >= piece.buf_in.width - radius || sv >= piece.buf_in.height - radius)
        continue;
      // only where the window actually carries pixels
      const int bx = su - roi_in.x, by = sv - roi_in.y;
      if(bx < 0 || by < 0 || bx >= roi_in.width || by >= roi_in.height) continue;
      const float *o = out + ((size_t)j * roi_out.width + i) * 4;
      assert_float_equal(o[0], 0.1f, 1e-4f);
      assert_float_equal(o[1], 0.8f, 1e-4f);
      assert_float_equal(o[2], 0.1f, 1e-4f);
      checked++;
    }
  assert_true(checked > roi_out.width * roi_out.height / 4);

  dt_free_align(out);
  dt_free_align(in);
}

int main(int argc, char *argv[])
{
  darktable.num_openmp_threads = 1;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_zoomed_viewport_covers_picture),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
