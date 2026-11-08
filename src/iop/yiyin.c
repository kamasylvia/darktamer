/*
    This file is part of darktable,
    Copyright (C) 2026 the darktamer developers.

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

// yiyin frame: expands the canvas around the picture and composites a
// backdrop (solid fill or blurred copy of the picture), a drop shadow and
// rounded corners, yiyin-style. The layout arithmetic mirrors the yiyin
// composition pipeline's pure steps (YiyinLayout.swift, float64, ceil/round
// quirks kept verbatim); the render legs mirror YiyinKernels.metal.
// Everything runs in this run's plane pixels: the picture is never
// rescaled, the canvas grows instead.

#include "bauhaus/bauhaus.h"
#include "common/gaussian.h"
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <math.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_yiyin_params_t)

typedef enum dt_iop_yiyin_backdrop_t
{
  DT_IOP_YIYIN_BACKDROP_SOLID = 0, // $DESCRIPTION: "solid color"
  DT_IOP_YIYIN_BACKDROP_BLUR  = 1, // $DESCRIPTION: "blurred picture"
  DT_IOP_YIYIN_BACKDROP_COUNT = 2
} dt_iop_yiyin_backdrop_t;

typedef struct dt_iop_yiyin_params_t
{
  // backdrop
  dt_iop_yiyin_backdrop_t backdrop; // $DEFAULT: DT_IOP_YIYIN_BACKDROP_SOLID $DESCRIPTION: "backdrop"
  float fill_color[3];              // $DEFAULT: 1.0 1.0 1.0 $DESCRIPTION: "fill color"
  float blur_amount;                // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 100.0 $DESCRIPTION: "blur strength"
  int adaptive_backdrop;            // $DEFAULT: 1 $DESCRIPTION: "dim a bright backdrop"
  // canvas
  float main_image_width_rate;      // $MIN: 20.0 $MAX: 100.0 $DEFAULT: 90.0 $DESCRIPTION: "picture width"
  float mini_top_bottom_margin;     // $MIN: 0.0 $MAX: 50.0 $DEFAULT: 0.0 $DESCRIPTION: "min top/bottom margin"
  int aspect_w;                     // $MIN: 0 $MAX: 10000 $DEFAULT: 0 $DESCRIPTION: "aspect width"
  int aspect_h;                     // $MIN: 0 $MAX: 10000 $DEFAULT: 0 $DESCRIPTION: "aspect height"
  int landscape_output;             // $DEFAULT: 0 $DESCRIPTION: "landscape output"
  // picture
  float corner_radius;              // $MIN: 0.0 $MAX: 50.0 $DEFAULT: 2.1 $DESCRIPTION: "corner radius"
  float shadow;                     // $MIN: 0.0 $MAX: 50.0 $DEFAULT: 6.0 $DESCRIPTION: "shadow"
  float text_bottom_offset;         // $MIN: 0.0 $MAX: 0.2 $DEFAULT: 0.027 $DESCRIPTION: "bottom reserve"
} dt_iop_yiyin_params_t;

typedef struct dt_iop_yiyin_data_t
{
  dt_iop_yiyin_backdrop_t backdrop;
  float fill_color[3];
  float blur_amount;
  int adaptive_backdrop;
  float main_image_width_rate;
  float mini_top_bottom_margin;
  int aspect_w;
  int aspect_h;
  int landscape_output;
  float corner_radius;
  float shadow;
  float text_bottom_offset;
} dt_iop_yiyin_data_t;

typedef struct dt_iop_yiyin_global_data_t
{
  int kernel_yiyin_composite;
  int kernel_yiyin_shadow_sdf;
  int kernel_yiyin_downsample;
} dt_iop_yiyin_global_data_t;

typedef struct dt_iop_yiyin_gui_data_t
{
  GtkWidget *backdrop;
  GtkWidget *fill_color;
  GtkWidget *blur_amount;
  GtkWidget *adaptive_backdrop;
  GtkWidget *main_image_width_rate;
  GtkWidget *mini_top_bottom_margin;
  GtkWidget *aspect_w;
  GtkWidget *aspect_h;
  GtkWidget *landscape_output;
  GtkWidget *corner_radius;
  GtkWidget *shadow;
} dt_iop_yiyin_gui_data_t;

// the joint layout, one computation per run. Pixel values are integers in
// the canvas frame (top-left origin, y down); float64 intermediates folded
// in exactly as yiyin does.
typedef struct _yiyin_layout_t
{
  int canvas_w, canvas_h;  // backdrop canvas dims
  int main_left, main_top; // picture origin on the canvas
  int content_h, content_top;
  double radius;      // corner radius in canvas px (0 = square)
  double shadow_blur; // shadow width in canvas px (CSS shadowBlur unit, a
                      // Gaussian sigma is half of it)
} _yiyin_layout_t;

// brightness tiers for the adaptive dim overlay (0-255 encoded scale);
// below thresholds map to the gray level used as the overlay color
static const float _yiyin_tiers[][2] =
{
  { 15.0f, 180.0f },
  { 20.0f, 158.0f },
  { 40.0f, 128.0f },
  { INFINITY, 0.0f },
};

// sRGB EOTF (IEC 61966-2-1), the probe/tier mapping lives in the encoded
// domain and comes back as a linear overlay color
static inline float _yiyin_srgb_decode(const float c)
{
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float _yiyin_srgb_encode(const float c)
{
  return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

// signed distance to a rounded rectangle; the arcTo rounded rectangle is
// this SDF's boundary
static inline float _yiyin_sd_rounded_rect(const float px, const float py,
                                           const float cx, const float cy,
                                           const float hx, const float hy,
                                           const float radius)
{
  const float qx = fabsf(px - cx) - hx + radius;
  const float qy = fabsf(py - cy) - hy + radius;
  const float ox = qx > 0.0f ? qx : 0.0f;
  const float oy = qy > 0.0f ? qy : 0.0f;
  const float inside = fminf(fmaxf(qx, qy), 0.0f);
  return sqrtf(ox * ox + oy * oy) + inside - radius;
}

// inside coverage with a 1px antialias band
static inline float _yiyin_rect_coverage(const float px, const float py,
                                         const float cx, const float cy,
                                         const float hx, const float hy,
                                         const float radius)
{
  const float sd = _yiyin_sd_rounded_rect(px, py, cx, cy, hx, hy, radius);
  const float t = CLAMP((sd + 0.5f), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep(-0.5, 0.5, sd)
}

// the enabled-neutral seed: rate 100, no margins, no aspect/landscape, no
// radius/shadow -- the canvas coincides with the picture. The identity is
// PARAM-based, never formula-based: the ceil-quirk makes e.g. a 7px-wide
// canvas at rate 100 grow to 8px.
static gboolean _yiyin_is_identity(const dt_iop_yiyin_data_t *d)
{
  return d->main_image_width_rate >= 100.0f
    && d->mini_top_bottom_margin <= 0.0f
    && !(d->aspect_w > 0 && d->aspect_h > 0)
    && !d->landscape_output
    && d->corner_radius <= 0.0f
    && d->shadow <= 0.0f;
}

static void _yiyin_layout(_yiyin_layout_t *L,
                          const int img_w, const int img_h,
                          const dt_iop_yiyin_data_t *d)
{
  const double w = img_w, h = img_h;

  // step 1: the aspect/landscape reset
  double reset_w = w, reset_h = h;
  if(d->aspect_w > 0 && d->aspect_h > 0)
  {
    const double rate = (double)d->aspect_w / (double)d->aspect_h;
    if(w >= h)
      reset_h = round(reset_w / rate);
    else
      reset_w = round(reset_h * rate);
  }
  else if(d->landscape_output && reset_w < reset_h)
  {
    const double t = reset_w;
    reset_w = reset_h;
    reset_h = t;
  }
  const double wh_rate = reset_w / reset_h;

  // step 2/4: canvas size, content-height driven, width grows when the
  // picture exceeds the width rate (called twice in yiyin; pass 1 sizes
  // with the picture height, pass 2 with the final content height)
  const double main_rate = d->main_image_width_rate / 100.0;
  double bg_h = h;                    // pass 1: content height = image height
  double bg_w = ceil(bg_h * wh_rate);
  if(w / bg_w > main_rate)
  {
    bg_w = ceil(w / main_rate);
    bg_h = ceil(bg_w / wh_rate);
  }

  // step 3: content height (margins >= shadow; rows consume 3/4 of the
  // offset plus the bottom slot -- the dt port has no text rows yet)
  const double bg_height = bg_h;
  const double top_offset = bg_height * (d->mini_top_bottom_margin / 100.0);
  double content_top = ceil(top_offset);
  double main_img_offset = content_top * 2.0;
  if(d->shadow > 0.0f)
  {
    const double shadow_height = ceil(h * (d->shadow / 100.0));
    if(shadow_height > content_top) content_top = shadow_height;
    main_img_offset = content_top * 2.0;
  }
  const double content_h = ceil(h + main_img_offset);

  // pass 2: the final canvas
  bg_h = content_h;
  bg_w = ceil(bg_h * wh_rate);
  if(w / bg_w > main_rate)
  {
    bg_w = ceil(w / main_rate);
    bg_h = ceil(bg_w / wh_rate);
  }

  // step 5: the picture centered on the final canvas
  const int main_left = (int)round((bg_w - w) / 2.0);
  const int main_top = (int)content_top + (int)round((bg_h - content_h) / 2.0);

  // step 6: corner radius / shadow blur, ceil(main height) BEFORE the
  // percent multiply
  const double main_h_ceiled = ceil(h);
  L->radius = d->corner_radius > 0.0f ? main_h_ceiled * (d->corner_radius / 100.0) : 0.0;
  L->shadow_blur = d->shadow > 0.0f ? main_h_ceiled * (d->shadow / 100.0) : 0.0;

  L->canvas_w = (int)bg_w;
  L->canvas_h = (int)bg_h;
  L->main_left = main_left;
  L->main_top = main_top;
  L->content_h = (int)content_h;
  L->content_top = (int)content_top;
}

const char *name()
{
  return _("yiyin frame");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("grow the canvas and add a backdrop, a shadow and rounded corners"),
                                _("creative"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

const char *aliases()
{
  return _("frame|border|matte|yiyin");
}

int flags()
{
  // canvas-expanding: one instance, never tiled -- the backdrop/shadow need
  // the whole picture context
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_EXPAND_ROI_IN;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_EFFECTS;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int default_colorspace(dt_iop_module_t *self,
                       dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
  self->default_enabled = FALSE;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  const dt_iop_yiyin_data_t *d = piece->data;
  *roi_out = *roi_in;

  if(_yiyin_is_identity(d)) return;

  _yiyin_layout_t L;
  _yiyin_layout(&L, roi_in->width, roi_in->height, d);

  roi_out->x = 0;
  roi_out->y = 0;
  roi_out->width = MAX(L.canvas_w, roi_in->width);
  roi_out->height = MAX(L.canvas_h, roi_in->height);
  // sanity guard: keep the canvas within a sane multiple of the picture
  roi_out->width = MIN(roi_out->width, roi_in->width * 8);
  roi_out->height = MIN(roi_out->height, roi_in->height * 8);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  // the picture is composited 1:1 and feeds the blur backdrop, so the
  // whole input plane is needed at the output's scale
  const float p_inw = (float)piece->buf_in.width * roi_out->scale;
  const float p_inh = (float)piece->buf_in.height * roi_out->scale;

  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = (int)roundf(p_inw);
  roi_in->height = (int)roundf(p_inh);

  roi_in->width = MAX(roi_in->width, 1);
  roi_in->height = MAX(roi_in->height, 1);
}

gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *points,
                           size_t points_count)
{
  const dt_iop_yiyin_data_t *d = piece->data;
  if(_yiyin_is_identity(d)) return TRUE;

  _yiyin_layout_t L;
  _yiyin_layout(&L, piece->buf_in.width, piece->buf_in.height, d);

  float *const pts = DT_IS_ALIGNED(points);
  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] += L.main_left;
    pts[i + 1] += L.main_top;
  }
  return TRUE;
}

gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *points,
                               size_t points_count)
{
  const dt_iop_yiyin_data_t *d = piece->data;
  if(_yiyin_is_identity(d)) return TRUE;

  _yiyin_layout_t L;
  _yiyin_layout(&L, piece->buf_in.width, piece->buf_in.height, d);

  float *const pts = DT_IS_ALIGNED(points);
  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] -= L.main_left;
    pts[i + 1] -= L.main_top;
  }
  return TRUE;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const dt_iop_yiyin_data_t *d = piece->data;
  dt_iop_image_fill(out, 0.0f, roi_out->width, roi_out->height, 1);
  if(_yiyin_is_identity(d))
  {
    memcpy(out, in, sizeof(float) * roi_in->width * roi_in->height);
    return;
  }

  _yiyin_layout_t L;
  _yiyin_layout(&L, roi_in->width, roi_in->height, d);
  // the full picture rect carries the mask (corner rounding is a render
  // detail, masks stay rectangular)
  const int bx = CLAMP(L.main_left, 0, roi_out->width - roi_in->width);
  const int by = CLAMP(L.main_top, 0, roi_out->height - roi_in->height);

  DT_OMP_FOR()
  for(int j = 0; j < roi_in->height; j++)
  {
    float *outb = out + (size_t)(j + by) * roi_out->width + bx;
    const float *inb = in + (size_t)j * roi_in->width;
    memcpy(outb, inb, sizeof(float) * roi_in->width);
  }
}

// the blur backdrop proxy: aspect-preserving box downsample of the picture,
// long edge capped (the yiyin genBlurImg square-stretch quirk is replaced
// by an aspect-preserving proxy + normalized sampling)
#define YIYIN_PROXY_LONG_EDGE 2048

static void _yiyin_proxy_dims(const int w, const int h, int *pw, int *ph)
{
  const int long_edge = MIN(YIYIN_PROXY_LONG_EDGE, MAX(w, h));
  if(MAX(w, h) <= long_edge)
  {
    *pw = w;
    *ph = h;
  }
  else
  {
    const double scale = (double)long_edge / MAX(w, h);
    *pw = MAX(1, (int)round(w * scale));
    *ph = MAX(1, (int)round(h * scale));
  }
}

static void _yiyin_box_downsample(const float *const in, const int sw, const int sh,
                                  float *const out, const int dw, const int dh)
{
  DT_OMP_FOR()
  for(int y = 0; y < dh; y++)
  {
    const int y0 = (int)floor((double)y * sh / dh);
    int y1 = (int)ceil((double)(y + 1) * sh / dh);
    y1 = MIN(y1, sh);
    const int ye = MAX(y0, y1 - 1);
    for(int x = 0; x < dw; x++)
    {
      const int x0 = (int)floor((double)x * sw / dw);
      int x1 = (int)ceil((double)(x + 1) * sw / dw);
      x1 = MIN(x1, sw);
      const int xe = MAX(x0, x1 - 1);
      float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int sy = y0; sy <= ye; sy++)
      {
        const float *src = in + ((size_t)sy * sw + (size_t)x0) * 4;
        for(int sx = x0; sx <= xe; sx++)
        {
          acc[0] += src[0];
          acc[1] += src[1];
          acc[2] += src[2];
          acc[3] += src[3];
          src += 4;
        }
      }
      const float count = (float)((xe - x0 + 1) * (ye - y0 + 1));
      float *dst = out + ((size_t)y * dw + (size_t)x) * 4;
      dst[0] = acc[0] / count;
      dst[1] = acc[1] / count;
      dst[2] = acc[2] / count;
      dst[3] = acc[3] / count;
    }
  }
}

// the brightness probe: 16x16 box average of the proxy, mapped back to the
// encoded scale for the tier selection; returns the linear overlay color
static void _yiyin_probe_overlay(const float *const proxy, const int pw, const int ph,
                                 const int adaptive, dt_aligned_pixel_t overlay,
                                 int *has_overlay)
{
  *has_overlay = 0;
  for(int c = 0; c < 4; c++) overlay[c] = 0.0f;
  if(!adaptive) return;

  // box average the proxy down to 16x16
  const int dw = 16, dh = 16;
  float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  for(int y = 0; y < dh; y++)
  {
    const int y0 = (int)floor((double)y * ph / dh);
    int y1 = (int)ceil((double)(y + 1) * ph / dh);
    y1 = MIN(y1, ph);
    const int ye = MAX(y0, y1 - 1);
    for(int x = 0; x < dw; x++)
    {
      const int x0 = (int)floor((double)x * pw / dw);
      int x1 = (int)ceil((double)(x + 1) * pw / dw);
      x1 = MIN(x1, pw);
      const int xe = MAX(x0, x1 - 1);
      for(int sy = y0; sy <= ye; sy++)
      {
        const float *src = proxy + ((size_t)sy * pw + (size_t)x0) * 4;
        for(int sx = x0; sx <= xe; sx++)
        {
          acc[0] += src[0];
          acc[1] += src[1];
          acc[2] += src[2];
          acc[3] += src[3];
          src += 4;
        }
      }
    }
  }
  const float count = (float)(dw * dh);
  const float mean[3] = { acc[0] / count, acc[1] / count, acc[2] / count };

  // mean (linear) -> encoded 0-255 scale -> brightness tier -> the gray
  // level as the overlay color, decoded back to linear
  const float brightness8
      = (_yiyin_srgb_encode(CLAMP(mean[0], 0.0f, 1.0f))
         + _yiyin_srgb_encode(CLAMP(mean[1], 0.0f, 1.0f))
         + _yiyin_srgb_encode(CLAMP(mean[2], 0.0f, 1.0f)))
        / 3.0f * 255.0f;

  float gray8 = 0.0f;
  const int n_tiers = sizeof(_yiyin_tiers) / sizeof(_yiyin_tiers[0]);
  for(int t = 0; t < n_tiers; t++)
  {
    if(brightness8 < _yiyin_tiers[t][0])
    {
      gray8 = _yiyin_tiers[t][1];
      break;
    }
  }
  if(gray8 <= 0.0f) return;

  const float g = _yiyin_srgb_decode(gray8 / 255.0f);
  overlay[0] = g;
  overlay[1] = g;
  overlay[2] = g;
  overlay[3] = 0.2f;
  *has_overlay = 1;
}

// bilinear clamp-to-edge sample of the proxy at normalized canvas coords
static void _yiyin_sample_proxy(const float *const proxy, const int pw, const int ph,
                                const float u, const float v, float *const out)
{
  const float fx = CLAMP(u * pw - 0.5f, 0.0f, (float)(pw - 1));
  const float fy = CLAMP(v * ph - 0.5f, 0.0f, (float)(ph - 1));
  const int x0 = (int)fx, y0 = (int)fy;
  const int x1 = MIN(x0 + 1, pw - 1), y1 = MIN(y0 + 1, ph - 1);
  const float dx = fx - x0, dy = fy - y0;
  const float *p00 = proxy + ((size_t)y0 * pw + x0) * 4;
  const float *p10 = proxy + ((size_t)y0 * pw + x1) * 4;
  const float *p01 = proxy + ((size_t)y1 * pw + x0) * 4;
  const float *p11 = proxy + ((size_t)y1 * pw + x1) * 4;
  for(int c = 0; c < 4; c++)
  {
    const float top = p00[c] * (1.0f - dx) + p10[c] * dx;
    const float bot = p01[c] * (1.0f - dx) + p11[c] * dx;
    out[c] = top * (1.0f - dy) + bot * dy;
  }
}

// the shadow plane: the rounded-rect SDF alpha stamped on a sub-plane that
// covers the picture rect plus the blur fringe, then Gaussian blurred
// (sigma = 0.5 * shadowBlur -- the CSS canvas shadowBlur unit is ~2 sigma)
static float *_yiyin_shadow_plane(const float *const in, const int iw, const int ih,
                                  const dt_iop_roi_t *const roi_out,
                                  const _yiyin_layout_t *L, const double sigma,
                                  int *sub_x, int *sub_y, int *sub_w, int *sub_h)
{
  const int margin = (int)ceil(3.0 * sigma + L->radius + 2.0);
  *sub_x = MAX(0, L->main_left - margin);
  *sub_y = MAX(0, L->main_top - margin);
  *sub_w = MIN(roi_out->width, L->main_left + iw + margin) - *sub_x;
  *sub_h = MIN(roi_out->height, L->main_top + ih + margin) - *sub_y;
  if(*sub_w <= 0 || *sub_h <= 0) return NULL;

  const float cx = L->main_left + iw * 0.5f;
  const float cy = L->main_top + ih * 0.5f;
  const float hx = iw * 0.5f, hy = ih * 0.5f;
  const float radius = (float)L->radius;

  const size_t n = (size_t)(*sub_w) * (*sub_h);
  float *plane = dt_alloc_align_float(n);
  if(!plane) return NULL;

  DT_OMP_FOR()
  for(int j = 0; j < *sub_h; j++)
  {
    const float py = j + *sub_y + 0.5f;
    for(int i = 0; i < *sub_w; i++)
    {
      const float px = i + *sub_x + 0.5f;
      plane[(size_t)j * (*sub_w) + i] =
        _yiyin_rect_coverage(px, py, cx, cy, hx, hy, radius);
    }
  }

  // dt_gaussian_init dereferences max/min unconditionally: alpha clamps
  // to [0, 1]
  const float smin[1] = { 0.0f };
  const float smax[1] = { 1.0f };
  dt_gaussian_t *g = dt_gaussian_init(*sub_w, *sub_h, 1, smax, smin, sigma, DT_IOP_GAUSSIAN_ZERO);
  if(!g)
  {
    dt_free_align(plane);
    return NULL;
  }
  dt_gaussian_blur(g, plane, plane);
  dt_gaussian_free(g);
  return plane;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_yiyin_data_t *const d = piece->data;
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(_yiyin_is_identity(d))
  {
    // the canvas coincides with the picture: byte-identical blit
    if(roi_out->width == roi_in->width && roi_out->height == roi_in->height)
      memcpy(out, in, sizeof(float) * (size_t)roi_out->width * roi_out->height * 4);
    else
      dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
    return;
  }

  _yiyin_layout_t L;
  _yiyin_layout(&L, roi_in->width, roi_in->height, d);
  const int cw = roi_out->width, ch = roi_out->height;

  // 1. the blur proxy (blur mode only)
  float *proxy = NULL;
  int pw = 0, ph = 0;
  if(d->backdrop == DT_IOP_YIYIN_BACKDROP_BLUR)
  {
    _yiyin_proxy_dims(roi_in->width, roi_in->height, &pw, &ph);
    proxy = dt_alloc_align_float((size_t)pw * ph * 4);
    if(proxy)
    {
      _yiyin_box_downsample(in, roi_in->width, roi_in->height, proxy, pw, ph);
      // sigma on the proxy: amount% * canvas height * 0.5, rescaled to the
      // proxy by the vertical mapping
      const double sigma_canvas = (d->blur_amount / 100.0) * ch * 0.5;
      const double sigma_proxy = sigma_canvas * ((double)ph / (double)ch);
      const float pmax[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
      const float pmin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      dt_gaussian_t *g = dt_gaussian_init(pw, ph, 4, pmax, pmin, sigma_proxy, DT_IOP_GAUSSIAN_ZERO);
      if(g)
      {
        dt_gaussian_blur(g, proxy, proxy);
        dt_gaussian_free(g);
      }
    }
  }

  // 2. the adaptive dim overlay (blur mode only)
  dt_aligned_pixel_t overlay = { 0.0f };
  int has_overlay = 0;
  if(d->backdrop == DT_IOP_YIYIN_BACKDROP_BLUR && proxy)
    _yiyin_probe_overlay(proxy, pw, ph, d->adaptive_backdrop, overlay, &has_overlay);

  // 3. the shadow plane
  float *shadow = NULL;
  int sub_x = 0, sub_y = 0, sub_w = 0, sub_h = 0;
  if(L.shadow_blur > 0.0)
  {
    const double sigma = L.shadow_blur * 0.5;
    shadow = _yiyin_shadow_plane(in, roi_in->width, roi_in->height, roi_out, &L, sigma,
                                 &sub_x, &sub_y, &sub_w, &sub_h);
  }

  // 4. composite: backdrop band + overlay + shadow + the picture clipped by
  //    the rounded-rect SDF (the picture lookup is exact 1:1)
  const float fill[4] = { d->fill_color[0], d->fill_color[1], d->fill_color[2], 1.0f };
  const float cx = L.main_left + roi_in->width * 0.5f;
  const float cy = L.main_top + roi_in->height * 0.5f;
  const float hx = roi_in->width * 0.5f, hy = roi_in->height * 0.5f;
  const float radius = (float)L.radius;

  DT_OMP_FOR()
  for(int j = 0; j < ch; j++)
  {
    const float py = j + 0.5f;
    float *o = out + (size_t)j * cw * 4;

    for(int i = 0; i < cw; i++)
    {
      const float px = i + 0.5f;

      // backdrop: solid fill or the blurred proxy stretched to the canvas
      if(proxy)
      {
        _yiyin_sample_proxy(proxy, pw, ph, (px / cw), (py / ch), o);
      }
      else
      {
        o[0] = fill[0];
        o[1] = fill[1];
        o[2] = fill[2];
        o[3] = 1.0f;
      }

      if(has_overlay)
      {
        o[0] = o[0] + (overlay[0] - o[0]) * overlay[3];
        o[1] = o[1] + (overlay[1] - o[1]) * overlay[3];
        o[2] = o[2] + (overlay[2] - o[2]) * overlay[3];
      }

      // shadow darkens beneath the picture
      if(shadow)
      {
        const int si = i - sub_x, sj = j - sub_y;
        if(si >= 0 && sj >= 0 && si < sub_w && sj < sub_h)
        {
          const float a = CLAMP(shadow[(size_t)sj * sub_w + si], 0.0f, 1.0f);
          o[0] *= 1.0f - a;
          o[1] *= 1.0f - a;
          o[2] *= 1.0f - a;
        }
      }

      // the picture, 1:1, rounded-rect clipped
      const int su = i - L.main_left, sv = j - L.main_top;
      if(su >= 0 && sv >= 0 && su < roi_in->width && sv < roi_in->height)
      {
        const float *m = in + ((size_t)sv * roi_in->width + su) * 4;
        const float cover = _yiyin_rect_coverage(px, py, cx, cy, hx, hy, radius);
        o[0] = o[0] + (m[0] - o[0]) * cover;
        o[1] = o[1] + (m[1] - o[1]) * cover;
        o[2] = o[2] + (m[2] - o[2]) * cover;
      }
      o[3] = 1.0f;
      o += 4;
    }
  }

  if(proxy) dt_free_align(proxy);
  if(shadow) dt_free_align(shadow);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_yiyin_data_t *d = piece->data;
  dt_iop_yiyin_global_data_t *gd = self->global_data;
  const int devid = piece->pipe->devid;
  cl_int err = CL_SUCCESS;

  if(_yiyin_is_identity(d))
  {
    const size_t region[3] = { roi_in->width, roi_in->height, 1 };
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN,
                                        region);
  }

  _yiyin_layout_t L;
  _yiyin_layout(&L, roi_in->width, roi_in->height, d);
  const int cw = roi_out->width, ch = roi_out->height;

  // composite/SDF geometry, output-plane px
  const float sdf_cx = L.main_left + roi_in->width * 0.5f;
  const float sdf_cy = L.main_top + roi_in->height * 0.5f;
  const float sdf_hx = roi_in->width * 0.5f;
  const float sdf_hy = roi_in->height * 0.5f;
  const float sdf_radius = (float)L.radius;
  const float main_offset_x = (float)L.main_left;
  const float main_offset_y = (float)L.main_top;
  const float main_size_w = (float)roi_in->width;
  const float main_size_h = (float)roi_in->height;

  // device temporaries, declared ahead of any goto that skips them
  cl_mem dev_proxy = NULL, dev_proxy2 = NULL, dev_shadow = NULL;

  // 1. the blur proxy (device side, blur mode only)
  int pw = 0, ph = 0;
  const int wants_blur = d->backdrop == DT_IOP_YIYIN_BACKDROP_BLUR;
  if(wants_blur)
  {
    _yiyin_proxy_dims(roi_in->width, roi_in->height, &pw, &ph);
    dev_proxy = dt_opencl_alloc_device(devid, pw, ph, 4 * sizeof(float));
    dev_proxy2 = dt_opencl_alloc_device(devid, pw, ph, 4 * sizeof(float));
    if(!dev_proxy || !dev_proxy2)
    {
      dt_print(DT_DEBUG_OPENCL, "[yiyin] can't allocate proxy buffers on gpu");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto finish;
    }
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_yiyin_downsample, pw, ph,
                                           CLARG(dev_in), CLARG(dev_proxy),
                                           CLARG(roi_in->width), CLARG(roi_in->height),
                                           CLARG(pw), CLARG(ph));
    if(err != CL_SUCCESS) goto finish;

    const double sigma_canvas = (d->blur_amount / 100.0) * ch * 0.5;
    const double sigma_proxy = sigma_canvas * ((double)ph / (double)ch);
    const float pmax[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
    const float pmin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, pw, ph, 4, pmax, pmin, sigma_proxy,
                                              DT_IOP_GAUSSIAN_ZERO);
    if(g)
    {
      err = dt_gaussian_blur_cl(g, dev_proxy, dev_proxy2);
      dt_gaussian_free_cl(g);
      if(err != CL_SUCCESS) goto finish;
      cl_mem t = dev_proxy;
      dev_proxy = dev_proxy2;
      dev_proxy2 = t;
    }
  }

  // 2. the adaptive dim overlay: a 16x16 box-average probe read back for
  //    the tier selection (the blur keeps the mean, so the proxy result is
  //    the same as the picture one)
  dt_aligned_pixel_t overlay = { 0.0f };
  int has_overlay = 0;
  if(wants_blur && d->adaptive_backdrop)
  {
    const int probe_w = 16, probe_h = 16;
    cl_mem dev_probe = dt_opencl_alloc_device(devid, probe_w, probe_h, 4 * sizeof(float));
    if(dev_probe)
    {
      cl_int e = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_yiyin_downsample,
                                                  probe_w, probe_h,
                                                  CLARG(dev_in), CLARG(dev_probe),
                                                  CLARG(roi_in->width), CLARG(roi_in->height),
                                                  CLARG(probe_w), CLARG(probe_h));
      if(e == CL_SUCCESS)
      {
        const size_t region[3] = { probe_w, probe_h, 1 };
        float probe[16 * 16 * 4];
        e = dt_opencl_read_host_from_image_raw(devid, probe, dev_probe, CLIMG_ORIGIN,
                                               region, 0, CL_TRUE);
        if(e == CL_SUCCESS)
        {
          float acc[3] = { 0.0f, 0.0f, 0.0f };
          for(int p = 0; p < probe_w * probe_h; p++)
          {
            acc[0] += probe[p * 4];
            acc[1] += probe[p * 4 + 1];
            acc[2] += probe[p * 4 + 2];
          }
          const float mean[3] = { acc[0] / (probe_w * probe_h),
                                  acc[1] / (probe_w * probe_h),
                                  acc[2] / (probe_w * probe_h) };
          const float brightness8
              = (_yiyin_srgb_encode(CLAMP(mean[0], 0.0f, 1.0f))
                 + _yiyin_srgb_encode(CLAMP(mean[1], 0.0f, 1.0f))
                 + _yiyin_srgb_encode(CLAMP(mean[2], 0.0f, 1.0f)))
                / 3.0f * 255.0f;
          const int n_tiers = sizeof(_yiyin_tiers) / sizeof(_yiyin_tiers[0]);
          float gray8 = 0.0f;
          for(int t = 0; t < n_tiers; t++)
          {
            if(brightness8 < _yiyin_tiers[t][0])
            {
              gray8 = _yiyin_tiers[t][1];
              break;
            }
          }
          if(gray8 > 0.0f)
          {
            const float gg = _yiyin_srgb_decode(gray8 / 255.0f);
            overlay[0] = gg;
            overlay[1] = gg;
            overlay[2] = gg;
            overlay[3] = 0.2f;
            has_overlay = 1;
          }
        }
      }
      dt_opencl_release_mem_object(dev_probe);
    }
  }

  // 3. the shadow plane (sub-plane around the picture rect, device side)
  int sub_x = 0, sub_y = 0, sub_w = 0, sub_h = 0;
  if(L.shadow_blur > 0.0)
  {
    const double sigma = L.shadow_blur * 0.5;
    const int margin = (int)ceil(3.0 * sigma + L.radius + 2.0);
    sub_x = MAX(0, L.main_left - margin);
    sub_y = MAX(0, L.main_top - margin);
    sub_w = MIN(cw, L.main_left + roi_in->width + margin) - sub_x;
    sub_h = MIN(ch, L.main_top + roi_in->height + margin) - sub_y;

    if(sub_w > 0 && sub_h > 0)
    {
      dev_shadow = dt_opencl_alloc_device(devid, sub_w, sub_h, 4 * sizeof(float));
      cl_mem dev_shadow2 = dt_opencl_alloc_device(devid, sub_w, sub_h, 4 * sizeof(float));
      if(!dev_shadow || !dev_shadow2)
      {
        if(dev_shadow2) dt_opencl_release_mem_object(dev_shadow2);
        dt_print(DT_DEBUG_OPENCL, "[yiyin] can't allocate shadow buffers on gpu");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto finish;
      }
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_yiyin_shadow_sdf, sub_w, sub_h,
                                             CLARG(dev_shadow),
                                             CLARG(sub_w), CLARG(sub_h),
                                             CLARG(sdf_cx), CLARG(sdf_cy),
                                             CLARG(sdf_hx), CLARG(sdf_hy),
                                             CLARG(sdf_radius),
                                             CLARG(sub_x), CLARG(sub_y));
      if(err != CL_SUCCESS)
      {
        dt_opencl_release_mem_object(dev_shadow2);
        goto finish;
      }
      const float smin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      const float smax[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, sub_w, sub_h, 4, smax, smin, sigma,
                                                DT_IOP_GAUSSIAN_ZERO);
      if(g)
      {
        err = dt_gaussian_blur_cl(g, dev_shadow, dev_shadow2);
        dt_gaussian_free_cl(g);
        dt_opencl_release_mem_object(dev_shadow2);
        if(err != CL_SUCCESS) goto finish;
      }
      else
      {
        dt_opencl_release_mem_object(dev_shadow2);
        dt_opencl_release_mem_object(dev_shadow);
        dev_shadow = NULL; // fall through: composite without the shadow
      }
    }
  }

  // 4. composite
  const int has_blur = wants_blur && dev_proxy ? 1 : 0;
  err = dt_opencl_enqueue_kernel_2d_args
    (devid, gd->kernel_yiyin_composite, cw, ch,
     CLARG(dev_in), CLARG(dev_proxy ? dev_proxy : dev_in), CLARG(dev_shadow ? dev_shadow : dev_in),
     CLARG(dev_out),
     CLARG(has_blur), CLARG(has_overlay),
     CLARG(dev_shadow ? 1 : 0),
     CLARG(d->fill_color[0]), CLARG(d->fill_color[1]), CLARG(d->fill_color[2]),
     CLARG(overlay[0]), CLARG(overlay[1]), CLARG(overlay[2]), CLARG(overlay[3]),
     CLARG(main_offset_x), CLARG(main_offset_y),
     CLARG(main_size_w), CLARG(main_size_h),
     CLARG(sdf_cx), CLARG(sdf_cy), CLARG(sdf_hx), CLARG(sdf_hy),
     CLARG(sdf_radius),
     CLARG(cw), CLARG(ch),
     CLARG(pw), CLARG(ph),
     CLARG(sub_x), CLARG(sub_y), CLARG(sub_w), CLARG(sub_h));

finish:
  if(dev_proxy) dt_opencl_release_mem_object(dev_proxy);
  if(dev_proxy2) dt_opencl_release_mem_object(dev_proxy2);
  if(dev_shadow) dt_opencl_release_mem_object(dev_shadow);
  return err;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  const int program = 53; // yiyin.cl, from programs.conf
  dt_iop_yiyin_global_data_t *gd = malloc(sizeof(dt_iop_yiyin_global_data_t));
  self->data = gd;
  gd->kernel_yiyin_composite = dt_opencl_create_kernel(program, "yiyin_composite");
  gd->kernel_yiyin_shadow_sdf = dt_opencl_create_kernel(program, "yiyin_shadow_sdf");
  gd->kernel_yiyin_downsample = dt_opencl_create_kernel(program, "yiyin_downsample");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_yiyin_global_data_t *gd = self->data;
  if(gd)
  {
    dt_opencl_free_kernel(gd->kernel_yiyin_composite);
    dt_opencl_free_kernel(gd->kernel_yiyin_shadow_sdf);
    dt_opencl_free_kernel(gd->kernel_yiyin_downsample);
  }
  free(self->data);
  self->data = NULL;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_yiyin_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

// gui

static void _fill_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_yiyin_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->fill_color[0] = c.red;
  p->fill_color[1] = c.green;
  p->fill_color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_yiyin_gui_data_t *g = self->gui_data;
  dt_iop_yiyin_params_t *p = self->params;

  if(fabsf(p->fill_color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->fill_color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->fill_color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  GdkRGBA c = (GdkRGBA){.red = self->picked_color[0],
                        .green = self->picked_color[1],
                        .blue = self->picked_color[2],
                        .alpha = 1.0 };

  p->fill_color[0] = self->picked_color[0];
  p->fill_color[1] = self->picked_color[1];
  p->fill_color[2] = self->picked_color[2];
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->fill_color), &c);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_yiyin_gui_data_t *g = self->gui_data;
  dt_iop_yiyin_params_t *p = self->params;

  dt_bauhaus_combobox_set(g->backdrop, p->backdrop);
  GdkRGBA c = { p->fill_color[0], p->fill_color[1], p->fill_color[2], 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->fill_color), &c);
  dt_bauhaus_slider_set(g->blur_amount, p->blur_amount);
  dt_bauhaus_toggle_set(g->adaptive_backdrop, p->adaptive_backdrop);
  dt_bauhaus_slider_set(g->main_image_width_rate, p->main_image_width_rate);
  dt_bauhaus_slider_set(g->mini_top_bottom_margin, p->mini_top_bottom_margin);
  dt_bauhaus_slider_set(g->aspect_w, p->aspect_w);
  dt_bauhaus_slider_set(g->aspect_h, p->aspect_h);
  dt_bauhaus_toggle_set(g->landscape_output, p->landscape_output);
  dt_bauhaus_slider_set(g->corner_radius, p->corner_radius);
  dt_bauhaus_slider_set(g->shadow, p->shadow);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_yiyin_gui_data_t *g = IOP_GUI_ALLOC(yiyin);

  g->backdrop = dt_bauhaus_combobox_from_params(self, "backdrop");
  gtk_widget_set_tooltip_text(g->backdrop, _("fill the backdrop band with a solid color or a blurred copy of the picture"));

  GdkRGBA color = { 1.0, 1.0, 1.0, 1.0 };
  g->fill_color = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->fill_color), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->fill_color), _("select fill color"));
  gtk_widget_set_tooltip_text(g->fill_color, _("the solid backdrop color"));
  g_signal_connect(G_OBJECT(g->fill_color), "color-set", G_CALLBACK(_fill_color_set), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->fill_color, TRUE, TRUE, 0);

  g->blur_amount = dt_bauhaus_slider_from_params(self, "blur_amount");
  dt_bauhaus_slider_set_format(g->blur_amount, "%");
  gtk_widget_set_tooltip_text(g->blur_amount, _("how strongly the blurred backdrop is blurred"));

  g->adaptive_backdrop = dt_bauhaus_toggle_from_params(self, "adaptive_backdrop");
  gtk_widget_set_tooltip_text(g->adaptive_backdrop, _("dim a bright blurred backdrop so the picture stands out"));

  g->main_image_width_rate = dt_bauhaus_slider_from_params(self, "main_image_width_rate");
  dt_bauhaus_slider_set_format(g->main_image_width_rate, "%");
  gtk_widget_set_tooltip_text(g->main_image_width_rate, _("the picture width as a share of the canvas width; the canvas grows so the picture never exceeds it"));

  g->mini_top_bottom_margin = dt_bauhaus_slider_from_params(self, "mini_top_bottom_margin");
  dt_bauhaus_slider_set_format(g->mini_top_bottom_margin, "%");
  gtk_widget_set_tooltip_text(g->mini_top_bottom_margin, _("a minimal margin above and below the picture"));

  g->aspect_w = dt_bauhaus_slider_from_params(self, "aspect_w");
  dt_bauhaus_slider_set_step(g->aspect_w, 1.0);
  dt_bauhaus_slider_set_digits(g->aspect_w, 0);
  gtk_widget_set_tooltip_text(g->aspect_w, _("canvas aspect width; 0 keeps the picture aspect"));

  g->aspect_h = dt_bauhaus_slider_from_params(self, "aspect_h");
  dt_bauhaus_slider_set_step(g->aspect_h, 1.0);
  dt_bauhaus_slider_set_digits(g->aspect_h, 0);
  gtk_widget_set_tooltip_text(g->aspect_h, _("canvas aspect height; 0 keeps the picture aspect"));

  g->landscape_output = dt_bauhaus_toggle_from_params(self, "landscape_output");
  gtk_widget_set_tooltip_text(g->landscape_output, _("swap the canvas to landscape orientation"));

  g->corner_radius = dt_bauhaus_slider_from_params(self, "corner_radius");
  dt_bauhaus_slider_set_format(g->corner_radius, "%");
  gtk_widget_set_tooltip_text(g->corner_radius, _("rounds the picture corners by a share of its height"));

  g->shadow = dt_bauhaus_slider_from_params(self, "shadow");
  dt_bauhaus_slider_set_format(g->shadow, "%");
  gtk_widget_set_tooltip_text(g->shadow, _("drop shadow width as a share of the picture height"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
