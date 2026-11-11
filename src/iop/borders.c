/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/gaussian.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/borders_helper.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#ifdef __APPLE__
#include <CoreText/CoreText.h>
#endif

DT_MODULE_INTROSPECTION(7, dt_iop_borders_params_t)

// Module constants
#define DT_IOP_BORDERS_ASPECT_IMAGE_VALUE 0.0f
#define DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE -1.0f
typedef enum dt_iop_orientation_t
{
  DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO = 0,      // $DESCRIPTION: "auto"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT = 1,  // $DESCRIPTION: "portrait"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE = 2, // $DESCRIPTION: "landscape"
} dt_iop_orientation_t;

// the frame mode: dt's plain border, the yiyin backdrop (blurred copy or
// solid fill, rounded corners, drop shadow, picture info rows) or a film
// strip. basic keeps the untouched legacy behaviour.
typedef enum dt_iop_borders_mode_t
{
  DT_IOP_BORDERS_MODE_BASIC = 0, // $DESCRIPTION: "basic"
  DT_IOP_BORDERS_MODE_BLUR  = 1, // $DESCRIPTION: "blur"
  DT_IOP_BORDERS_MODE_FILM  = 2, // $DESCRIPTION: "film strip"
  DT_IOP_BORDERS_MODE_COUNT = 3
} dt_iop_borders_mode_t;

// the backdrop of the blur mode (the yiyin solid_bg toggle)
typedef enum dt_iop_borders_backdrop_t
{
  DT_IOP_BORDERS_BACKDROP_SOLID = 0, // $DESCRIPTION: "solid color"
  DT_IOP_BORDERS_BACKDROP_BLUR  = 1, // $DESCRIPTION: "blurred picture"
  DT_IOP_BORDERS_BACKDROP_COUNT = 2
} dt_iop_borders_backdrop_t;

typedef enum dt_iop_basis_t
{
  DT_IOP_BORDERS_BASIS_AUTO = 0,     // $DESCRIPTION: "auto"
  DT_IOP_BORDERS_BASIS_WIDTH = 1,    // $DESCRIPTION: "width"
  DT_IOP_BORDERS_BASIS_HEIGHT = 2,   // $DESCRIPTION: "height"
  DT_IOP_BORDERS_BASIS_SHORTER = 3,  // $DESCRIPTION: "shorter"
  DT_IOP_BORDERS_BASIS_LONGER = 4,   // $DESCRIPTION: "longer"
} dt_iop_basis_t;

static const float _aspect_ratios[]
  = { DT_IOP_BORDERS_ASPECT_IMAGE_VALUE,
      3.0f, 95.0f / 33.0f, 2.39f, 2.0f, 16.0f / 9.0f, 5.0f / 3.0f, 14.0f / 8.5f, PHI, 16.0f / 10.0f,
      3.0f / 2.0f, 297.0f / 210.0f, M_SQRT2, 7.0f / 5.0f, 4.0f / 3.0f, 11.0f / 8.5f, 14.0f / 11.0f,
      5.0f / 4.0f, 1.0f, DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE };
static const float _pos_h_ratios[] = { 0.5f, 1.0f / 3.0f, 3.0f / 8.0f, 5.0f / 8.0f, 2.0f / 3.0f };
static const float _pos_v_ratios[] = { 0.5f, 1.0f / 3.0f, 3.0f / 8.0f, 5.0f / 8.0f, 2.0f / 3.0f };

#define DT_IOP_BORDERS_ASPECT_COUNT G_N_ELEMENTS(_aspect_ratios)
#define DT_IOP_BORDERS_ASPECT_IMAGE_IDX 0
#define DT_IOP_BORDERS_ASPECT_CONSTANT_IDX (DT_IOP_BORDERS_ASPECT_COUNT - 1)
#define DT_IOP_BORDERS_POSITION_H_COUNT G_N_ELEMENTS(_pos_h_ratios)
#define DT_IOP_BORDERS_POSITION_V_COUNT G_N_ELEMENTS(_pos_v_ratios)

typedef struct dt_iop_borders_params_t
{
  float color[3];           // border color $DEFAULT: 1.0 $DESCRIPTION: "border color"
  float aspect;             /* aspect ratio of the outer frame w/h
                               $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
  char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                               DEFAULT: "constant border" */
  dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                               $DEFAULT: 0 $DESCRIPTION: "orientation" */
  float size;               /* border width relative to the length of the chosen basis
                               $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
  float pos_h;              /* picture horizontal position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
  char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float pos_v;              /* picture vertical position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
  char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float frame_size;         /* frame line width relative to border width
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
  float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
  float frame_color[3];     // frame line color $DEFAULT: 0.0 $DESCRIPTION: "frame line color"
  gboolean max_border_size; /* the way border size is computed
                               $DEFAULT: TRUE */
  dt_iop_basis_t basis;     /* side of the photo to use as basis for the size calculation
                               $DEFAULT: 0 $DESCRIPTION: "basis" */
  // ── v5: frame modes (basic / blur / film strip) ──
  dt_iop_borders_mode_t mode;         // $DEFAULT: DT_IOP_BORDERS_MODE_BLUR $DESCRIPTION: "mode"
  // blur mode (yiyin backdrop; the fill comes from color above)
  dt_iop_borders_backdrop_t backdrop; // $DEFAULT: DT_IOP_BORDERS_BACKDROP_BLUR $DESCRIPTION: "backdrop"
  float blur_amount;                  /* blurred backdrop strength
                               $MIN: 0.0 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "blur strength" */
  gboolean adaptive_backdrop; /* dim a bright blurred backdrop
                               $DEFAULT: TRUE $DESCRIPTION: "dim a bright backdrop" */
  float main_image_width_rate;        /* picture width as a share of the canvas width
                               $MIN: 20.0 $MAX: 100.0 $DEFAULT: 90.0 $DESCRIPTION: "picture width" */
  float mini_top_bottom_margin;       /* minimal margin above and below the picture
                               $MIN: 0.0 $MAX: 50.0 $DEFAULT: 10.0 $DESCRIPTION: "min top/bottom margin" */
  int aspect_w;             /* canvas aspect width; 0 keeps the picture aspect
                               $MIN: 0 $MAX: 10000 $DEFAULT: 0 $DESCRIPTION: "aspect width" */
  int aspect_h;             /* canvas aspect height; 0 keeps the picture aspect
                               $MIN: 0 $MAX: 10000 $DEFAULT: 0 $DESCRIPTION: "aspect height" */
  gboolean landscape_output; /* swap the canvas to landscape orientation
                               $DEFAULT: FALSE $DESCRIPTION: "landscape output" */
  float corner_radius;      /* rounds the picture corners by a share of its height
                               $MIN: 0.0 $MAX: 50.0 $DEFAULT: 2.1 $DESCRIPTION: "corner radius" */
  float shadow;             /* drop shadow width as a share of the picture height
                               $MIN: 0.0 $MAX: 50.0 $DEFAULT: 6.0 $DESCRIPTION: "shadow" */
  float text_bottom_offset; /* bottom text slot as a share of the canvas height
                               $MIN: 0.0 $MAX: 0.2 $DEFAULT: 0.027 $DESCRIPTION: "bottom reserve" */
  gboolean text_show;       /* print the picture info below the picture
                               $DEFAULT: TRUE $DESCRIPTION: "picture info rows" */
  char text_model[64];      /* picture info row 1 DEFAULT: "{Make} {Model}" */
  char text_params[96];     /* picture info row 2 */
  float text_size;          /* info text size as a share of the canvas width
                               $MIN: 1.0 $MAX: 6.0 $DEFAULT: 2.2 $DESCRIPTION: "info text size" */
  // film strip mode
  float film_band;          /* black band width above and below the picture
                               $MIN: 0.0 $MAX: 30.0 $DEFAULT: 12.0 $DESCRIPTION: "film band width" */
  char film_caption[64];    /* film edge marking DEFAULT: "KODAK PORTRA 400" */
  float film_caption_size;  /* edge marking size as a share of the band width
                               $MIN: 20.0 $MAX: 80.0 $DEFAULT: 50.0 $DESCRIPTION: "film text size" */
  // ── v6: configurable rows, fonts and film text color ──
  int text_rows;            /* how many info rows to print (0-3)
                               $MIN: 0 $MAX: 3 $DEFAULT: 3 $DESCRIPTION: "info row count" */
  char text_lens[64];       /* picture info row 2 DEFAULT: "{Lens}" */
  char text_font[64];       /* info rows font, a PostScript name DEFAULT: "Sans Bold" */
  char film_font[64];       /* film edge marking font, a PostScript name DEFAULT: "Sans Bold" */
  float film_caption_color[3]; /* film edge marking color $DEFAULT: 1.0 $DESCRIPTION: "film text color" */
  float text_size1;         /* info row 1 size as a share of the canvas width
                               $MIN: 0.5 $MAX: 6.0 $DEFAULT: 1.8 $DESCRIPTION: "row 1 size" */
  float text_size2;         /* info row 2 size as a share of the canvas width
                               $MIN: 0.5 $MAX: 6.0 $DEFAULT: 1.8 $DESCRIPTION: "row 2 size" */
  float text_size3;         /* info row 3 size as a share of the canvas width
                               $MIN: 0.5 $MAX: 6.0 $DEFAULT: 2.2 $DESCRIPTION: "row 3 size" */
  float text_opacity;       /* info text opacity
                               $MIN: 0.0 $MAX: 100.0 $DEFAULT: 90.0 $DESCRIPTION: "info text opacity" */
} dt_iop_borders_params_t;

typedef struct dt_iop_borders_gui_data_t
{
  GtkWidget *basis;
  GtkWidget *size;
  GtkWidget *aspect;
  GtkWidget *aspect_slider;
  GtkWidget *aspect_orient;
  GtkWidget *pos_h;
  GtkWidget *pos_h_slider;
  GtkWidget *pos_v;
  GtkWidget *pos_v_slider;
  GtkWidget *colorpick;
  GtkWidget *border_picker; // the 1st button
  GtkWidget *frame_size;
  GtkWidget *frame_offset;
  GtkWidget *frame_colorpick;
  GtkWidget *frame_picker; // the 2nd button
  GtkWidget *frame_color_box;
  GtkWidget *colorpick_box;
  // mode + blur/film widgets
  GtkWidget *mode;
  GtkWidget *backdrop;
  GtkWidget *blur_amount;
  GtkWidget *adaptive_backdrop;
  GtkWidget *main_image_width_rate;
  GtkWidget *mini_top_bottom_margin;
  GtkWidget *aspect_w;
  GtkWidget *aspect_h;
  GtkWidget *landscape_output;
  GtkWidget *corner_radius;
  GtkWidget *shadow;
  GtkWidget *text_show;
  GtkWidget *text_model;
  GtkWidget *text_lens;
  GtkWidget *text_params;
  GtkWidget *text_rows;
  GtkWidget *text_font;
  GtkWidget *text_size1;
  GtkWidget *text_size2;
  GtkWidget *text_size3;
  GtkWidget *text_opacity;
  GtkWidget *film_band;
  GtkWidget *film_caption;
  GtkWidget *film_font;
  GtkWidget *film_colorpick;
  GtkWidget *film_color_box;
  GtkWidget *film_caption_size;
} dt_iop_borders_gui_data_t;

// ******* Check and update legacy params...(esp. ver 4)
int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_borders_params_v3_t
  {
    float color[3];           // border color $DEFAULT: 1.0
    float aspect;             /* aspect ratio of the outer frame w/h
                               $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
    char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                                 DEFAULT: "constant border" */
    dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                                                  $DEFAULT: 0 $DESCRIPTION: "orientation" */
    float size;               /* border width relative to overall frame width
                                 $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
    float pos_h;              /* picture horizontal position ratio into the final image
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
    char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                                 DEFAULT: "1/2" */
    float pos_v;              /* picture vertical position ratio into the final image
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
    char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                                 DEFAULT: "1/2" */
    float frame_size;         /* frame line width relative to border width
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
    float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
    float frame_color[3];     // frame line color $DEFAULT: 0.0
    gboolean max_border_size; /* the way border size is computed
                                 $DEFAULT: TRUE */
  } dt_iop_borders_params_v3_t;

  dt_iop_borders_params_v3_t default_v3 =
    { { 1.0f, 1.0f, 1.0f },
      DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE,
      "                   ",
      0,
      0.1f,
      0.5f,
      "                   ",
      0.5f,
      "                   ",
      0.0f,
      0.5f,
      { 0.0f, 0.0f, 0.0f },
      TRUE
    };

  if(old_version == 1)
  {
    typedef struct dt_iop_borders_params_v1_t
    {
      float color[3]; // border color
      float aspect;   // aspect ratio of the outer frame w/h
      float size;     // border width relative to overall frame width
    } dt_iop_borders_params_v1_t;

    const dt_iop_borders_params_v1_t *o = (dt_iop_borders_params_v1_t *)old_params;
    dt_iop_borders_params_v3_t *n = calloc(1, sizeof(dt_iop_borders_params_v3_t));

    *n = default_v3; // start with a fresh copy of default parameters
    memcpy(n->color, o->color, sizeof(o->color));
    n->aspect = (o->aspect < 1) ? 1 / o->aspect : o->aspect;
    // no auto orientation in legacy param due to already convert aspect ratio
    n->aspect_orient = o->aspect > 1 ? DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE
                                     : DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT;
    n->size = fabsf(o->size); // no negative size any more (was for "constant border" detect)
    n->max_border_size = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_borders_params_v2_t
    {
      float color[3];       // border color
      float aspect;         // aspect ratio of the outer frame w/h
      char aspect_text[20]; // aspect ratio of the outer frame w/h (user string version)
      int aspect_orient;    // aspect ratio orientation
      float size;           // border width relative to overall frame width
      float pos_h;          // picture horizontal position ratio into the final image
      char pos_h_text[20];  // picture horizontal position ratio into the final image (user string version)
      float pos_v;          // picture vertical position ratio into the final image
      char pos_v_text[20];  // picture vertical position ratio into the final image (user string version)
      float frame_size;     // frame line width relative to border width
      float frame_offset;   // frame offset from picture size relative to [border width - frame width]
      float frame_color[3]; // frame line color
    } dt_iop_borders_params_v2_t;

    const dt_iop_borders_params_v2_t *o = (dt_iop_borders_params_v2_t *)old_params;
    dt_iop_borders_params_v3_t *n = calloc(1, sizeof(dt_iop_borders_params_v3_t));

    memcpy(n, o, sizeof(struct dt_iop_borders_params_v2_t));
    n->max_border_size = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 3)
  {
    typedef struct dt_iop_borders_params_v4_t
    {
      float color[3];           // border color $DEFAULT: 1.0
      float aspect;             /* aspect ratio of the outer frame w/h
                                $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
      char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                                  DEFAULT: "constant border" */
      dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                                                    $DEFAULT: 0 $DESCRIPTION: "orientation" */
      float size;               /* border width relative to overall frame width
                                  $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
      float pos_h;              /* picture horizontal position ratio into the final image
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
      char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                                  DEFAULT: "1/2" */
      float pos_v;              /* picture vertical position ratio into the final image
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
      char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                                  DEFAULT: "1/2" */
      float frame_size;         /* frame line width relative to border width
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
      float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
      float frame_color[3];     // frame line color $DEFAULT: 0.0
      gboolean max_border_size; /* the way border size is computed
                                  $DEFAULT: TRUE */
      dt_iop_basis_t basis;     /* side of the photo to use as basis for the size calculation
                                  $DEFAULT: 0 $DESCRIPTION: "basis" */
    } dt_iop_borders_params_v4_t;

    const dt_iop_borders_params_v3_t *o = (dt_iop_borders_params_v3_t *)old_params;
    dt_iop_borders_params_v4_t *n = calloc(1, sizeof(dt_iop_borders_params_v4_t));

    memcpy(n, o, sizeof(struct dt_iop_borders_params_v3_t));

    if(n->aspect == DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE && !n->max_border_size)
    {
      // the legacy behaviour is, when a constant border is used and the
      // max_border_size flag is set, the width is always used as basis.
      n->basis = DT_IOP_BORDERS_BASIS_WIDTH;
    }
    else
    {
      n->basis = DT_IOP_BORDERS_BASIS_AUTO;
    }

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v4_t);
    *new_version = 4;
    return 0;
  }

  // v4 -> v5: the frame modes are appended after the legacy fields; every
  // pre-existing edit stays a plain basic border
  if(old_version == 4)
  {
    typedef struct dt_iop_borders_params_v4_t
    {
      float color[3];
      float aspect;
      char aspect_text[20];
      dt_iop_orientation_t aspect_orient;
      float size;
      float pos_h;
      char pos_h_text[20];
      float pos_v;
      char pos_v_text[20];
      float frame_size;
      float frame_offset;
      float frame_color[3];
      gboolean max_border_size;
      dt_iop_basis_t basis;
    } dt_iop_borders_params_v4_t;

    dt_iop_borders_params_t *n = calloc(1, sizeof(dt_iop_borders_params_t));
    memcpy(n, old_params, sizeof(dt_iop_borders_params_v4_t));
    n->mode = DT_IOP_BORDERS_MODE_BASIC;
    n->backdrop = DT_IOP_BORDERS_BACKDROP_SOLID;
    n->blur_amount = 100.0f;
    n->adaptive_backdrop = 1;
    n->main_image_width_rate = 90.0f;
    n->mini_top_bottom_margin = 0.0f;
    n->aspect_w = n->aspect_h = 0;
    n->landscape_output = 0;
    n->corner_radius = 2.1f;
    n->shadow = 6.0f;
    n->text_bottom_offset = 0.027f;
    n->text_show = 0;
    g_strlcpy(n->text_model, "{Make} {Model}", sizeof(n->text_model));
    g_strlcpy(n->text_params, "{FocalLengthIn35mmFormat}mm f/{FNumber} {ExposureTime}s ISO{ISO}",
              sizeof(n->text_params));
    n->text_size = 2.2f;
    n->film_band = 12.0f;
    g_strlcpy(n->film_caption, "KODAK PORTRA 400", sizeof(n->film_caption));
    n->film_caption_size = 50.0f;
    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_t);
    *new_version = 5;
    return 0;
  }

  // v5 -> v6: rows/fonts/film color appended; a v5 edit that kept the old
  // defaults moves to the new defaults (blurred backdrop, info rows on)
  if(old_version == 5)
  {
    typedef struct dt_iop_borders_params_v5_t
    {
      float color[3];
      float aspect;
      char aspect_text[20];
      dt_iop_orientation_t aspect_orient;
      float size;
      float pos_h;
      char pos_h_text[20];
      float pos_v;
      char pos_v_text[20];
      float frame_size;
      float frame_offset;
      float frame_color[3];
      gboolean max_border_size;
      dt_iop_basis_t basis;
      dt_iop_borders_mode_t mode;
      dt_iop_borders_backdrop_t backdrop;
      float blur_amount;
      gboolean adaptive_backdrop;
      float main_image_width_rate;
      float mini_top_bottom_margin;
      int aspect_w;
      int aspect_h;
      gboolean landscape_output;
      float corner_radius;
      float shadow;
      float text_bottom_offset;
      gboolean text_show;
      char text_model[64];
      char text_params[96];
      float text_size;
      float film_band;
      char film_caption[64];
      float film_caption_size;
    } dt_iop_borders_params_v5_t;

    const dt_iop_borders_params_v5_t *o = old_params;
    dt_iop_borders_params_t *n = calloc(1, sizeof(dt_iop_borders_params_t));
    memcpy(n, o, sizeof(dt_iop_borders_params_v5_t));
    if(n->backdrop == DT_IOP_BORDERS_BACKDROP_SOLID)
      n->backdrop = DT_IOP_BORDERS_BACKDROP_BLUR;
    n->text_show = TRUE;
    n->text_rows = 3;
    g_strlcpy(n->text_lens, "{Lens}", sizeof(n->text_lens));
    g_strlcpy(n->text_font, "Sans Bold", sizeof(n->text_font));
    g_strlcpy(n->film_font, "Sans Bold", sizeof(n->film_font));
    n->film_caption_color[0] = n->film_caption_color[1] = n->film_caption_color[2] = 1.0f;
    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_t);
    *new_version = 6;
    return 0;
  }

  // v6 -> v7: the single info text size splits into three per-row sizes
  // (the old leading 1.36x becomes row 1's own size) and an opacity joins
  if(old_version == 6)
  {
    typedef struct dt_iop_borders_params_v6_t
    {
      float color[3];
      float aspect;
      char aspect_text[20];
      dt_iop_orientation_t aspect_orient;
      float size;
      float pos_h;
      char pos_h_text[20];
      float pos_v;
      char pos_v_text[20];
      float frame_size;
      float frame_offset;
      float frame_color[3];
      gboolean max_border_size;
      dt_iop_basis_t basis;
      dt_iop_borders_mode_t mode;
      dt_iop_borders_backdrop_t backdrop;
      float blur_amount;
      gboolean adaptive_backdrop;
      float main_image_width_rate;
      float mini_top_bottom_margin;
      int aspect_w;
      int aspect_h;
      gboolean landscape_output;
      float corner_radius;
      float shadow;
      float text_bottom_offset;
      gboolean text_show;
      char text_model[64];
      char text_params[96];
      float text_size;
      float film_band;
      char film_caption[64];
      float film_caption_size;
      int text_rows;
      char text_lens[64];
      char text_font[64];
      char film_font[64];
      float film_caption_color[3];
    } dt_iop_borders_params_v6_t;

    const dt_iop_borders_params_v6_t *o = old_params;
    dt_iop_borders_params_t *n = calloc(1, sizeof(dt_iop_borders_params_t));

    // everything before the split copies in order
    memcpy(n, o, offsetof(dt_iop_borders_params_v6_t, text_size));
    g_strlcpy(n->text_model, o->text_model, sizeof(n->text_model));
    g_strlcpy(n->text_params, o->text_params, sizeof(n->text_params));
    g_strlcpy(n->text_font, o->text_font, sizeof(n->text_font));
    n->text_size1 = o->text_size * 1.36f;
    n->text_size2 = o->text_size;
    n->text_size3 = o->text_size;
    n->text_opacity = 100.0f;
    n->film_band = o->film_band;
    g_strlcpy(n->film_caption, o->film_caption, sizeof(n->film_caption));
    n->film_caption_size = o->film_caption_size;
    n->text_rows = o->text_rows;
    g_strlcpy(n->text_lens, o->text_lens, sizeof(n->text_lens));
    g_strlcpy(n->film_font, o->film_font, sizeof(n->film_font));
    memcpy(n->film_caption_color, o->film_caption_color, sizeof(n->film_caption_color));
    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_t);
    *new_version = 7;
    return 0;
  }

  return 1;
}

typedef struct dt_iop_borders_global_data_t
{
  int kernel_borders_fill;
  // blur/film render legs (yiyin.cl, program 53)
  int kernel_yiyin_composite;
  int kernel_yiyin_shadow_sdf;
  int kernel_yiyin_downsample;
  int kernel_yiyin_film_composite;
  int kernel_yiyin_text_over;
} dt_iop_borders_global_data_t;


typedef struct dt_iop_borders_params_t dt_iop_borders_data_t;

// a rendered text-row bitmap: white glyph coverage in the alpha channel,
// rgb unwritten (the composite mixes toward white by the coverage)
typedef struct _yiyin_text_row_t
{
  float *coverage; // w*h float coverage plane, NULL when the row is empty
  int w, h;        // bitmap dims
} _yiyin_text_row_t;

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
  double text_bottom; // the bottom text slot in canvas px
} _yiyin_layout_t;

// the film strip geometry (fixed hole proportions, yiyin-free)
typedef struct _yiyin_film_geom_t
{
  int band;            // band height in px
  int hole_h, hole_w;  // hole dims
  int pitch;           // hole pitch (width + gap)
  int first_hole;      // first hole's left edge
  int hole_y0, hole_y1; // hole vertical range within each band
  double black[3];     // band fill, linear
  double hole[3];      // hole fill (light-box shine), linear
} _yiyin_film_geom_t;

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
static gboolean _yiyin_is_identity(const dt_iop_borders_data_t *d)
{
  return d->main_image_width_rate >= 100.0f
    && d->mini_top_bottom_margin <= 0.0f
    && !(d->aspect_w > 0 && d->aspect_h > 0)
    && !d->landscape_output
    && d->corner_radius <= 0.0f
    && d->shadow <= 0.0f
    && !d->text_show;
}

// steps 1-2 pass 1: the font-sizing canvas (yiyin clacBgImgSize() default
// height -- row-height independent, which is why fonts can be sized before
// the rows are known)
static void _yiyin_pass1_canvas(const int img_w, const int img_h,
                                const dt_iop_borders_data_t *d, int *pw, int *ph)
{
  const double w = img_w, h = img_h;
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
  const double main_rate = d->main_image_width_rate / 100.0;
  double bg_h = h;
  double bg_w = ceil(bg_h * wh_rate);
  if(w / bg_w > main_rate)
  {
    bg_w = ceil(w / main_rate);
    bg_h = ceil(bg_w / wh_rate);
  }
  *pw = (int)bg_w;
  *ph = (int)bg_h;
}

// yiyin-style layout. rows_w/rows_h = the measured text-row dims in canvas
// px (top-most row first, yiyin template list order); zero rows when the
// info rows are off. rows_w drives the canvas width: a text row wider than
// the picture grows the frame rather than being clipped. The film strip
// layout never goes through here.
static void _yiyin_layout(_yiyin_layout_t *L,
                          const int img_w, const int img_h,
                          const dt_iop_borders_data_t *d,
                          const int *const rows_w, const int *const rows_h,
                          const int n_rows)
{
  const double w = img_w, h = img_h;
  int max_row_w = 0;
  for(int r = 0; r < n_rows; r++)
    if(rows_w[r] > max_row_w) max_row_w = rows_w[r];

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

  // step 3: content height (margins >= shadow; text rows consume 3/4 of
  // the offset plus the bottom slot)
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
  const double text_bottom_px = bg_height * d->text_bottom_offset;
  double text_h = 0.0;
  for(int r = 0; r < n_rows; r++) text_h += (double)rows_h[r];
  if(n_rows > 0)
  {
    // yiyin :526-529 -- JS float math kept verbatim
    main_img_offset = main_img_offset * 3.0 / 4.0;
    main_img_offset += text_bottom_px;
  }
  const double content_h = ceil(text_h + h + main_img_offset);

  // pass 2: the final canvas
  bg_h = content_h;
  bg_w = ceil(bg_h * wh_rate);
  if(w / bg_w > main_rate)
  {
    bg_w = ceil(w / main_rate);
    bg_h = ceil(bg_w / wh_rate);
  }

  // the text rows may be wider than the picture: widen the canvas (and
  // re-derive the height from the same aspect) so nothing is clipped
  if(max_row_w > bg_w)
  {
    bg_w = (double)max_row_w;
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
  L->text_bottom = text_bottom_px;
}

// the film strip geometry: picture 1:1 between two black bands with a
// centered row of rectangular light-box holes and the edge marking text
static void _yiyin_film_geom(_yiyin_film_geom_t *G, const int img_w, const int img_h,
                             const dt_iop_borders_data_t *d)
{
  G->band = MAX(1, (int)round((double)img_h * (d->film_band / 100.0)));
  G->hole_h = MAX(1, (int)round(G->band * 0.40));
  G->hole_w = MAX(1, (int)round(G->hole_h * 1.35));
  G->pitch = (int)round(G->hole_w * 2.2);
  const double gap = G->pitch - G->hole_w;
  // as many holes as fit, the whole row centered
  const int n = MAX(1, (int)floor(((double)img_w + gap) / G->pitch));
  const int total = (int)round(n * (double)G->pitch - gap);
  G->first_hole = MAX(0, (int)round(((double)img_w - total) / 2.0));
  G->hole_y0 = (int)round(G->band * 0.30);
  G->hole_y1 = G->hole_y0 + G->hole_h;
  // near-black band (#141412) and the light-box shine through the holes
  G->black[0] = G->black[1] = G->black[2] = _yiyin_srgb_decode(0.078f);
  G->hole[0] = G->hole[1] = G->hole[2] = 0.85;
}

// ── picture info rows + film edge marking (pango text legs) ──────────────

// append one placeholder's value; returns FALSE when the picture carries no
// value for it (a manual lens reports focal 0 / f-number 0, a body may miss
// tags). dt stores no explicit 35mm-equivalent focal, so that placeholder
// derives it from the focal length times the crop factor.
static gboolean _yiyin_exif_value(GString *out, const char *key, const dt_image_t *img)
{
  if(!g_strcmp0(key, "Make"))
  {
    if(!img->exif_maker[0]) return FALSE;
    g_string_append(out, img->exif_maker);
  }
  else if(!g_strcmp0(key, "Model"))
  {
    if(!img->exif_model[0]) return FALSE;
    g_string_append(out, img->exif_model);
  }
  else if(!g_strcmp0(key, "Lens"))
  {
    // a manual lens reports a placeholder model of bare dashes; treat it
    // as no lens rather than printing "----" in the frame
    if(!img->exif_lens[0]
       || strspn(img->exif_lens, "-") == strlen(img->exif_lens))
      return FALSE;
    g_string_append(out, img->exif_lens);
  }
  else if(!g_strcmp0(key, "FocalLength"))
  {
    if(img->exif_focal_length <= 0.0f) return FALSE;
    g_string_append_printf(out, "%.0f", (double)img->exif_focal_length);
  }
  else if(!g_strcmp0(key, "FocalLengthIn35mmFormat"))
  {
    const double crop = img->exif_crop > 0.01f ? (double)img->exif_crop : 1.0;
    const double focal35 = (double)img->exif_focal_length * crop;
    if(focal35 <= 0.0) return FALSE;
    g_string_append_printf(out, "%.0f", focal35);
  }
  else if(!g_strcmp0(key, "FNumber"))
  {
    if(img->exif_aperture <= 0.0f) return FALSE;
    g_string_append_printf(out, "%.1f", (double)img->exif_aperture);
  }
  else if(!g_strcmp0(key, "ExposureTime"))
  {
    if(img->exif_exposure <= 0.0f) return FALSE;
    char *e = dt_util_format_exposure(img->exif_exposure);
    g_string_append(out, e);
    g_free(e);
  }
  else if(!g_strcmp0(key, "ISO"))
  {
    if(img->exif_iso <= 0.0f) return FALSE;
    g_string_append_printf(out, "%.0f", (double)img->exif_iso);
  }
  else
  {
    // an unknown placeholder stays visible: a silent drop would hide a typo
    g_string_append_c(out, '{');
    g_string_append(out, key);
    g_string_append_c(out, '}');
  }
  return TRUE;
}

// expand one yiyin template pattern against the picture's exif. A
// space-separated token holding an unavailable placeholder is dropped whole,
// so a manual-lens shot prints "1/1000s ISO200" rather than "mm f/0.0 ...".
static char *_yiyin_expand_pattern(const char *pattern, const dt_image_t *img)
{
  GString *result = g_string_sized_new(128);
  const char *p = pattern;
  gboolean first_token = TRUE;
  while(*p)
  {
    while(*p == ' ') p++;
    if(!*p) break;
    const char *tok_end = strchr(p, ' ');
    if(!tok_end) tok_end = p + strlen(p);

    GString *tok = g_string_sized_new(32);
    gboolean complete = TRUE;
    const char *q = p;
    while(q < tok_end)
    {
      if(*q != '{')
      {
        g_string_append_c(tok, *q++);
        continue;
      }
      const char *end = memchr(q, '}', tok_end - q);
      if(!end) break; // unclosed brace: the literal tail is dropped
      char key[32];
      const size_t klen = MIN((size_t)(end - q - 1), sizeof(key) - 1);
      memcpy(key, q + 1, klen);
      key[klen] = '\0';
      if(!_yiyin_exif_value(tok, key, img)) complete = FALSE;
      q = end + 1;
    }

    if(complete && tok->len > 0)
    {
      if(!first_token) g_string_append_c(result, ' ');
      g_string_append_len(result, tok->str, tok->len);
      first_token = FALSE;
    }
    g_string_free(tok, TRUE);
    p = tok_end;
  }
  return g_string_free(result, FALSE);
}

// cairo/pixman refuse image surfaces past their dimension limits, and the
// failure path hands back an error surface whose data pointer is NULL.
// Nothing here may exceed it, however long the pattern text gets.
#define YIYIN_TEXT_MAX_DIM 16000

// the pango layout for one text row; metrics and render MUST share it, or
// the measured row and the rendered bitmap drift apart by a pixel and the
// canvas reserved for the text misses it
// resolve a font the way code editors do: by PostScript name. On macOS the
// name goes through CoreText, which knows the system fonts by their PS
// identifiers ("STHeitiSC-Light", "KaitiSC-Regular", "PingFangSC-Semibold",
// ...), and the family plus weight traits come back out as a Pango
// description. Without CoreText the name is handed to Pango verbatim.
static void _yiyin_resolve_font(const char *name, char *out, const int out_len)
{
  snprintf(out, out_len, "%s", (name && *name) ? name : "Sans Bold");
#ifdef __APPLE__
  CFStringRef ps = CFStringCreateWithCString(kCFAllocatorDefault, out, kCFStringEncodingUTF8);
  CTFontRef font = CTFontCreateWithName(ps, 12.0, NULL);
  CFRelease(ps);

  CFStringRef fam_ref = CTFontCopyFamilyName(font);
  char family[128] = "";
  const Boolean got_fam = CFStringGetCString(fam_ref, family, sizeof(family),
                                             kCFStringEncodingUTF8);
  CFRelease(fam_ref);
  if(!got_fam || !family[0]) { CFRelease(font); return; }

  char style[24] = "";
  if(CTFontGetSymbolicTraits(font) & kCTFontTraitBold)
    snprintf(style, sizeof(style), " Bold");
  else
  {
    float w = -1.0f;
    CFDictionaryRef tr = CTFontCopyTraits(font);
    if(tr)
    {
      CFNumberRef wn = CFDictionaryGetValue(tr, kCTFontWeightTrait);
      if(wn) CFNumberGetValue(wn, kCFNumberFloat32Type, &w);
      CFRelease(tr);
    }
    if(w <= -0.3f)      snprintf(style, sizeof(style), " Light");
    else if(w >= 0.6f)  snprintf(style, sizeof(style), " Heavy");
    else if(w >= 0.4f)  snprintf(style, sizeof(style), " Semibold");
    else if(w >= 0.15f) snprintf(style, sizeof(style), " Medium");
  }
  CFRelease(font);
  snprintf(out, out_len, "%s%s", family, style);
#endif
}

static PangoLayout *_yiyin_text_layout_new(const char *text, const double size_px,
                                           const char *fontdesc,
                                           PangoContext **ctx_out,
                                           PangoFontDescription **fd_out)
{
  char resolved[128];
  _yiyin_resolve_font(fontdesc, resolved, sizeof(resolved));
  PangoFontMap *fontmap = pango_cairo_font_map_get_default();
  PangoContext *context = pango_font_map_create_context(fontmap);
  PangoLayout *layout = pango_layout_new(context);
  PangoFontDescription *fd
    = pango_font_description_from_string(resolved);
  pango_font_description_set_absolute_size(fd, (gint)(size_px * PANGO_SCALE));
  pango_layout_set_font_description(layout, fd);
  pango_layout_set_text(layout, text, -1);
  *ctx_out = context;
  *fd_out = fd;
  return layout;
}

static void _yiyin_text_layout_free(PangoLayout *layout, PangoContext *context,
                                    PangoFontDescription *fd)
{
  g_object_unref(layout);
  g_object_unref(context);
  pango_font_description_free(fd);
}

// measure one text row without rasterizing it: the ROI planner runs on
// every pipe evaluation and must not allocate a surface for the text it is
// only reserving room for. This reports the TRUE size even past the surface
// limit: the planner has to see the overrun to scale the row back down. Only
// a size the measurement cannot produce at all (no text, degenerate font)
// reports zero.
static void _yiyin_text_metrics(const char *text, const double size_px,
                                const char *fontdesc, int *w, int *h)
{
  *w = *h = 0;
  if(!text || !*text || size_px < 2.0 || size_px > 1.0e6) return;

  PangoContext *context = NULL;
  PangoFontDescription *fd = NULL;
  PangoLayout *layout = _yiyin_text_layout_new(text, size_px, fontdesc, &context, &fd);
  int tw = 0, th = 0;
  pango_layout_get_pixel_size(layout, &tw, &th);
  _yiyin_text_layout_free(layout, context, fd);
  if(tw < 1 || th < 1) return;
  *w = tw + 2;
  *h = th + 2;
}

// render one text line into a white-glyph coverage bitmap. The pango/cairo
// pair below is premultiplied ARGB32 with white glyphs, whose un-premultiplied
// encoded color is exactly 1.0 -- so the composite mix toward linear white by
// the ALPHA alone is the correct encoded/linear algebra for white text, the
// yiyin_watermark_row semantics with the color side collapsed.
static void _yiyin_render_text_row(const char *text, const double size_px,
                                   const char *fontdesc, _yiyin_text_row_t *row)
{
  row->coverage = NULL;
  row->w = row->h = 0;
  if(!text || !*text || size_px < 2.0 || size_px > YIYIN_TEXT_MAX_DIM) return;

  PangoContext *context = NULL;
  PangoFontDescription *fd = NULL;
  PangoLayout *layout = _yiyin_text_layout_new(text, size_px, fontdesc, &context, &fd);

  int tw = 0, th = 0;
  pango_layout_get_pixel_size(layout, &tw, &th);
  if(tw < 1 || th < 1 || tw > YIYIN_TEXT_MAX_DIM || th > YIYIN_TEXT_MAX_DIM)
  {
    _yiyin_text_layout_free(layout, context, fd);
    return;
  }
  const int w = tw + 2, h = th + 2;

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if(cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surf);
    _yiyin_text_layout_free(layout, context, fd);
    return;
  }
  cairo_t *cr = cairo_create(surf);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_move_to(cr, 1.0, 1.0);
  pango_cairo_show_layout(cr, layout);
  cairo_destroy(cr);
  cairo_surface_flush(surf);

  // an error surface has no data: the guard above catches the dimension
  // cases, this one catches whatever cairo refuses for its own reasons
  const guint8 *data = cairo_image_surface_get_data(surf);
  float *cov = data ? dt_alloc_align_float((size_t)w * h) : NULL;
  if(cov)
  {
    const int stride = cairo_image_surface_get_stride(surf);
    for(int y = 0; y < h; y++)
    {
      const guint8 *src = data + (size_t)y * stride;
      float *dst = cov + (size_t)y * w;
      for(int x = 0; x < w; x++)
        dst[x] = src[x * 4 + 3] / 255.0f; // alpha = the coverage
    }
    row->coverage = cov;
    row->w = w;
    row->h = h;
  }
  cairo_surface_destroy(surf);
  _yiyin_text_layout_free(layout, context, fd);
}

static void _yiyin_free_text_row(_yiyin_text_row_t *row)
{
  if(row->coverage) dt_free_align(row->coverage);
  row->coverage = NULL;
  row->w = row->h = 0;
}

// the row planner: both the ROI sizing and the render must agree on the
// font sizes, the expanded texts and the measured metrics, so this is the
// ONE place that decides them. `metrics` receives the measured row dims
// (list order); widths also feed the canvas width so an overlong row widens
// the frame instead of being clipped.
#define YIYIN_MAX_ROWS 3

typedef struct _yiyin_rows_plan_t
{
  char *text[YIYIN_MAX_ROWS];
  double font_px[YIYIN_MAX_ROWS]; // the size each row is MEASURED and RENDERED at
  int w[YIYIN_MAX_ROWS], h[YIYIN_MAX_ROWS];
  int n;
  int max_w;
} _yiyin_rows_plan_t;

// a text row is fitted to the picture's own width, which the yiyin canvas
// is always at least as wide as: the text shrinks to the frame rather than
// the frame growing to a paragraph, so the reserved canvas stays bounded
// however much text is pasted in
#define YIYIN_ROW_MAX_RATE 1.0

static void _yiyin_plan_rows(_yiyin_rows_plan_t *plan, const int img_w, const int img_h,
                             const dt_iop_borders_data_t *d, const dt_image_t *img)
{
  memset(plan, 0, sizeof(*plan));
  if(!d->text_show || d->backdrop != DT_IOP_BORDERS_BACKDROP_BLUR) return;

  int pw1 = 0, ph1 = 0;
  _yiyin_pass1_canvas(img_w, img_h, d, &pw1, &ph1);
  const int max_row_w = MAX(64, (int)((double)img_w * YIYIN_ROW_MAX_RATE));

  plan->n = CLAMP(d->text_rows, 0, YIYIN_MAX_ROWS);
  if(plan->n == 0) return;
  plan->text[0] = _yiyin_expand_pattern(d->text_model, img);
  plan->text[1] = plan->n > 1 ? _yiyin_expand_pattern(d->text_lens, img) : NULL;
  plan->text[2] = plan->n > 2 ? _yiyin_expand_pattern(d->text_params, img) : NULL;
  const float row_size[YIYIN_MAX_ROWS] = { d->text_size1, d->text_size2, d->text_size3 };
  for(int r = 0; r < plan->n; r++) plan->font_px[r] = (double)pw1 * (row_size[r] / 100.0);

  for(int r = 0; r < plan->n; r++)
  {
    _yiyin_text_metrics(plan->text[r], plan->font_px[r], d->text_font, &plan->w[r], &plan->h[r]);
    // shrink to fit the cap rather than dropping the row. Advance is not
    // perfectly linear in the size, so re-measure and repeat until the row
    // actually fits (each pass moves the estimate closer; the loop is
    // bounded in case a font refuses to cooperate).
    for(int pass = 0; pass < 8 && plan->w[r] > max_row_w && plan->w[r] > 0; pass++)
    {
      const double scale = (double)max_row_w / (double)plan->w[r] * 0.98;
      plan->font_px[r] = MAX(2.0, plan->font_px[r] * scale);
      _yiyin_text_metrics(plan->text[r], plan->font_px[r], d->text_font, &plan->w[r], &plan->h[r]);
    }
    if(plan->w[r] > plan->max_w) plan->max_w = plan->w[r];
  }
}

static void _yiyin_free_rows_plan(_yiyin_rows_plan_t *plan)
{
  for(int r = 0; r < plan->n; r++)
  {
    g_free(plan->text[r]);
    plan->text[r] = NULL;
  }
  plan->n = 0;
}

// the yiyin bottom-slot stacking (composite :300-319 + the :543-545
// last-row inflation quirk): the LIST-LAST row sits at the canvas bottom,
// every earlier row above at round(prevTop - h). row_top[i] receives each
// input row's top (list order).
static void _yiyin_stack_text_rows(const _yiyin_text_row_t *rows, const int n_rows,
                                   const int canvas_w, const int canvas_h,
                                   const double bottom_offset_px,
                                   int *const row_left, int *const row_top)
{
  int prev_top = 0;
  for(int index = 0; index < n_rows; index++)
  {
    // reversed() order: the list-LAST row is index 0 (bottom-most)
    const int r = n_rows - 1 - index;
    const double height_d = (double)rows[r].h + (index == 0 ? bottom_offset_px : 0.0);
    const int top = index == 0
      ? (int)round((double)canvas_h - height_d)
      : (int)round((double)prev_top - height_d);
    const int left = (int)round(((double)canvas_w - (double)rows[r].w) / 2.0);
    row_left[r] = left;
    row_top[r] = top;
    prev_top = top;
  }
}

// blend the rendered rows onto the plane (in-place, linear white by the
// coverage); rows whose bitmap leaves the canvas are clipped
static void _yiyin_composite_text_rows(float *const plane, const int plane_w, const int plane_h,
                                       const _yiyin_text_row_t *rows, const int n_rows,
                                       const int *const row_left, const int *const row_top,
                                       const float *const text_color,
                                       const float text_opacity)
{
  for(int r = 0; r < n_rows; r++)
  {
    const _yiyin_text_row_t *row = &rows[r];
    if(!row->coverage) continue;
    const int x0 = MAX(0, row_left[r]);
    const int y0 = MAX(0, row_top[r]);
    const int x1 = MIN(plane_w, row_left[r] + row->w);
    const int y1 = MIN(plane_h, row_top[r] + row->h);
    DT_OMP_FOR()
    for(int y = y0; y < y1; y++)
    {
      const float *src = row->coverage + (size_t)(y - row_top[r]) * row->w;
      float *dst = plane + ((size_t)y * plane_w + x0) * 4;
      for(int x = x0; x < x1; x++)
      {
        const float a = src[x - row_left[r]] * CLAMP(text_opacity, 0.0f, 1.0f);
        dst[0] += (text_color[0] - dst[0]) * a;
        dst[1] += (text_color[1] - dst[1]) * a;
        dst[2] += (text_color[2] - dst[2]) * a;
        dst += 4;
      }
    }
  }
}

// the film strip: picture 1:1 between two black bands, a centered row of
// light-box holes per band, the edge marking centered in the bottom band
static void _yiyin_process_film(const float *const in, float *const out,
                                const dt_iop_roi_t *const roi_in,
                                const dt_iop_roi_t *const roi_out,
                                const dt_iop_borders_data_t *d)
{
  // the band is a property of the whole picture; scale it into this plane
  _yiyin_film_geom_t G;
  _yiyin_film_geom(&G, roi_in->width, roi_in->height, d);
  const int cw = roi_out->width, ch = roi_out->height;
  const int ih = roi_in->height;

  DT_OMP_FOR()
  for(int j = 0; j < ch; j++)
  {
    float *o = out + (size_t)j * cw * 4;
    const int pic_y = j - G.band;
    const gboolean in_picture = pic_y >= 0 && pic_y < ih;
    // in a band: the local y inside that band (top band and bottom band use
    // the same hole range)
    const int band_ly = j < G.band ? j : j - G.band - ih;
    const gboolean hole_row = !in_picture && band_ly >= G.hole_y0 && band_ly < G.hole_y1;
    const double *hole = G.hole, *black = G.black;

    for(int i = 0; i < cw; i++)
    {
      if(in_picture)
      {
        const float *m = in + ((size_t)pic_y * roi_in->width + i) * 4;
        o[0] = m[0];
        o[1] = m[1];
        o[2] = m[2];
      }
      else
      {
        const gboolean in_hole = hole_row && i >= G.first_hole
          && ((i - G.first_hole) % G.pitch) < G.hole_w;
        const double *c = in_hole ? hole : black;
        o[0] = c[0];
        o[1] = c[1];
        o[2] = c[2];
      }
      o[3] = 1.0f;
      o += 4;
    }
  }

  // the edge marking, centered in the bottom band
  if(d->film_caption[0])
  {
    _yiyin_text_row_t cap;
    const double font_px = (double)G.band * (d->film_caption_size / 100.0);
    _yiyin_render_text_row(d->film_caption, font_px, d->film_font, &cap);
    if(cap.coverage)
    {
      const int left = MAX(0, (cw - cap.w) / 2);
      const int top = G.band + ih + MAX(0, (G.band - cap.h) / 2);
      _yiyin_composite_text_rows(out, cw, ch, &cap, 1, &left, &top, d->film_caption_color, 1.0f);
      _yiyin_free_text_row(&cap);
    }
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
// `pic_x`/`pic_y` are the picture's origin INSIDE THIS OUTPUT PLANE (canvas
// origin scaled, minus the plane's canvas offset) and `radius` is already in
// plane pixels -- passing raw canvas values here is what misaligned the
// shadow once the viewport could move.
static float *_yiyin_shadow_plane(const int pic_x, const int pic_y,
                                  const int iw, const int ih,
                                  const int plane_w, const int plane_h,
                                  const float radius, const double sigma,
                                  int *sub_x, int *sub_y, int *sub_w, int *sub_h)
{
  const int margin = (int)ceil(3.0 * sigma + radius + 2.0);
  *sub_x = MAX(0, pic_x - margin);
  *sub_y = MAX(0, pic_y - margin);
  *sub_w = MIN(plane_w, pic_x + iw + margin) - *sub_x;
  *sub_h = MIN(plane_h, pic_y + ih + margin) - *sub_y;
  if(*sub_w <= 0 || *sub_h <= 0) return NULL;

  const float cx = pic_x + iw * 0.5f;
  const float cy = pic_y + ih * 0.5f;
  const float hx = iw * 0.5f, hy = ih * 0.5f;

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

// the blur-mode render leg (yiyin backdrop): proxy + dim overlay + shadow +
// the rounded picture, plus the info rows. The caller has dispatched the
// identity case; this always composes.
static void _yiyin_render_blur(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid,
                               void *const ovoid,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out,
                               const dt_iop_borders_data_t *d)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  // the picture info rows: the planner sizes the fonts and measures the
  // texts, the render rasterizes them from the SAME plan so the reserved
  // canvas and the drawn bitmaps cannot disagree
  _yiyin_text_row_t rows[YIYIN_MAX_ROWS];
  for(int r = 0; r < YIYIN_MAX_ROWS; r++)
  {
    rows[r].coverage = NULL;
    rows[r].w = rows[r].h = 0;
  }
  int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
  // the plan must describe the WHOLE picture: it is what the canvas was
  // sized for, and a viewport-sized plan would change the text size and the
  // reserved slot on every zoom step
  const int pic_w_full = piece->buf_in.width;
  const int pic_h_full = piece->buf_in.height;
  const float sc = roi_out->scale;
  _yiyin_rows_plan_t plan;
  _yiyin_plan_rows(&plan, pic_w_full, pic_h_full, d, &piece->pipe->image);
  const int n_rows = plan.n;
  if(n_rows)
  {
    for(int r = 0; r < n_rows; r++)
    {
      _yiyin_render_text_row(plan.text[r], plan.font_px[r] * sc, d->text_font, &rows[r]);
      rows_w[r] = rows[r].coverage ? (int)roundf(rows[r].w / sc) : 0;
      rows_h[r] = rows[r].coverage ? (int)roundf(rows[r].h / sc) : 0;
    }
  }
  _yiyin_free_rows_plan(&plan);

  _yiyin_layout_t L;
  _yiyin_layout(&L, roi_in->width, roi_in->height, d, rows_w, rows_h, n_rows);
  const int cw = roi_out->width, ch = roi_out->height;

  // 1. the blur proxy (blur mode only)
  float *proxy = NULL;
  int pw = 0, ph = 0;
  if(d->backdrop == DT_IOP_BORDERS_BACKDROP_BLUR)
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
  if(d->backdrop == DT_IOP_BORDERS_BACKDROP_BLUR && proxy)
    _yiyin_probe_overlay(proxy, pw, ph, d->adaptive_backdrop, overlay, &has_overlay);

  // geometry, output-plane px: canvas values scaled into this plane, then
  // shifted by the plane's own canvas offset
  const int pic_x = (int)roundf(L.main_left * sc) - roi_out->x;
  const int pic_y = (int)roundf(L.main_top * sc) - roi_out->y;
  const float cx = pic_x + roi_in->width * 0.5f;
  const float cy = pic_y + roi_in->height * 0.5f;
  const float hx = roi_in->width * 0.5f, hy = roi_in->height * 0.5f;
  const float radius = (float)(L.radius * sc);

  // 3. the shadow plane (plane-relative, radius already scaled)
  const float fill[4] = { d->color[0], d->color[1], d->color[2], 1.0f };
  float *shadow = NULL;
  int sub_x = 0, sub_y = 0, sub_w = 0, sub_h = 0;
  if(L.shadow_blur > 0.0)
  {
    const double sigma = L.shadow_blur * 0.5 * sc;
    shadow = _yiyin_shadow_plane(pic_x, pic_y, roi_in->width, roi_in->height,
                                 roi_out->width, roi_out->height,
                                 radius, sigma, &sub_x, &sub_y, &sub_w, &sub_h);
  }

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
      const int su = i - pic_x, sv = j - pic_y;
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

  // 5. the picture info rows, stacked bottom-up into the bottom slot
  if(n_rows)
  {
    int row_left[YIYIN_MAX_ROWS], row_top[YIYIN_MAX_ROWS];
    // the rows stack against the CANVAS (scaled), not this viewport; shifting
    // them by the plane's canvas offset is what keeps them in the frame
    const int canvas_w_s = (int)roundf(L.canvas_w * sc);
    const int canvas_h_s = (int)roundf(L.canvas_h * sc);
    _yiyin_stack_text_rows(rows, n_rows, canvas_w_s, canvas_h_s,
                           L.text_bottom * sc, row_left, row_top);
    for(int r = 0; r < n_rows; r++)
    {
      row_left[r] -= roi_out->x;
      row_top[r] -= roi_out->y;
    }
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    _yiyin_composite_text_rows(out, cw, ch, rows, n_rows, row_left, row_top, white,
                               d->text_opacity / 100.0f);
  }
  for(int r = 0; r < 2; r++) _yiyin_free_text_row(&rows[r]);

  if(proxy) dt_free_align(proxy);
  if(shadow) dt_free_align(shadow);
}


#ifdef HAVE_OPENCL


// pack the placed rows into one float4 coverage texture (each row blitted
// to its own offset inside the union bbox) and return the bbox destination
// on the plane
static void _yiyin_pack_text_tex(const _yiyin_text_row_t *rows, const int *const row_left,
                                 const int *const row_top, const int n,
                                 float **tex_out, int *tex_w, int *tex_h,
                                 int *dest_x, int *dest_y)
{
  *tex_out = NULL;
  int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
  gboolean any = FALSE;
  for(int r = 0; r < n; r++)
  {
    if(!rows[r].coverage) continue;
    any = TRUE;
    min_x = MIN(min_x, row_left[r]);
    min_y = MIN(min_y, row_top[r]);
    max_x = MAX(max_x, row_left[r] + rows[r].w);
    max_y = MAX(max_y, row_top[r] + rows[r].h);
  }
  if(!any || max_x <= min_x || max_y <= min_y) return;
  const int tw = max_x - min_x, th = max_y - min_y;
  float *tex = calloc((size_t)tw * th * 4, sizeof(float));
  if(!tex) return;
  for(int r = 0; r < n; r++)
  {
    if(!rows[r].coverage) continue;
    const int ox = row_left[r] - min_x, oy = row_top[r] - min_y;
    for(int y = 0; y < rows[r].h; y++)
    {
      const float *src = rows[r].coverage + (size_t)y * rows[r].w;
      float *dst = tex + ((size_t)(y + oy) * tw + ox) * 4;
      for(int x = 0; x < rows[r].w; x++)
      {
        dst[0] = src[x];
        dst[1] = src[x];
        dst[2] = src[x];
        dst[3] = 1.0f;
        dst += 4;
      }
    }
  }
  *tex_out = tex;
  *tex_w = tw;
  *tex_h = th;
  *dest_x = min_x;
  *dest_y = min_y;
}

static cl_mem _yiyin_upload_tex(const int devid, const float *tex, const int tw, const int th)
{
  cl_mem m = dt_opencl_alloc_device(devid, tw, th, 4 * sizeof(float));
  if(m && dt_opencl_write_host_to_image(devid, tex, m, tw, th, 4 * sizeof(float)) != CL_SUCCESS)
  {
    dt_opencl_release_mem_object(m);
    m = NULL;
  }
  return m;
}

// composite the text texture over a bbox of the plane: copy the bbox aside,
// run the text_over kernel writing back into the plane, release the copy
static cl_int _yiyin_cl_text_over(dt_iop_borders_global_data_t *gd, const int devid,
                                  cl_mem dev_out, cl_mem dev_tex,
                                  const int tex_w, const int tex_h,
                                  const int dest_x, const int dest_y,
                                  const int plane_w, const int plane_h,
                                  const float *const text_color,
                                  const float text_opacity)
{
  const int bx0 = MAX(0, dest_x), by0 = MAX(0, dest_y);
  const int bx1 = MIN(plane_w, dest_x + tex_w), by1 = MIN(plane_h, dest_y + tex_h);
  if(bx1 <= bx0 || by1 <= by0) return CL_SUCCESS;
  const int bw = bx1 - bx0, bh = by1 - by0;
  cl_mem dev_tmp = dt_opencl_alloc_device(devid, bw, bh, 4 * sizeof(float));
  if(!dev_tmp) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  const size_t src_origin[3] = { bx0, by0, 0 };
  const size_t dst_origin[3] = { 0, 0, 0 };
  const size_t region[3] = { bw, bh, 1 };
  cl_int err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, src_origin, dst_origin,
                                            region);
  if(err == CL_SUCCESS)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_yiyin_text_over, bw, bh,
                                           CLARG(dev_tmp), CLARG(dev_out), CLARG(dev_tex),
                                           CLARG(bx0), CLARG(by0),
                                           CLARG(dest_x), CLARG(dest_y),
                                           CLARG(tex_w), CLARG(tex_h),
                                           CLARG(text_color[0]), CLARG(text_color[1]),
                                           CLARG(text_color[2]), CLARG(text_opacity));
  dt_opencl_release_mem_object(dev_tmp);
  return err;
}

// the film strip on device: one geometric composite pass, then the edge
// marking text rendered on the host and composited over the bottom band
static cl_int _yiyin_process_film_cl(dt_iop_borders_global_data_t *gd, const int devid,
                                     cl_mem dev_in, cl_mem dev_out,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out,
                                     const dt_iop_borders_data_t *d)
{
  _yiyin_film_geom_t G;
  _yiyin_film_geom(&G, roi_in->width, roi_in->height, d);
  const int cw = roi_out->width, ch = roi_out->height;
  const float black_r = (float)G.black[0], black_g = (float)G.black[1], black_b = (float)G.black[2];
  const float hole_r = (float)G.hole[0], hole_g = (float)G.hole[1], hole_b = (float)G.hole[2];

  cl_int err = dt_opencl_enqueue_kernel_2d_args
    (devid, gd->kernel_yiyin_film_composite, cw, ch,
     CLARG(dev_in), CLARG(dev_out),
     CLARG(roi_in->width), CLARG(roi_in->height),
     CLARG(cw), CLARG(ch),
     CLARG(G.band), CLARG(G.hole_y0), CLARG(G.hole_y1),
     CLARG(G.hole_w), CLARG(G.pitch), CLARG(G.first_hole),
     CLARG(black_r), CLARG(black_g), CLARG(black_b),
     CLARG(hole_r), CLARG(hole_g), CLARG(hole_b));
  if(err != CL_SUCCESS) return err;

  if(d->film_caption[0])
  {
    _yiyin_text_row_t cap;
    const double font_px = (double)G.band * (d->film_caption_size / 100.0);
    _yiyin_render_text_row(d->film_caption, font_px, d->film_font, &cap);
    if(cap.coverage)
    {
      const int left = MAX(0, (cw - cap.w) / 2);
      const int top = G.band + roi_in->height + MAX(0, (G.band - cap.h) / 2);
      float *tex = NULL;
      int tex_w = 0, tex_h = 0, tex_x = 0, tex_y = 0;
      _yiyin_pack_text_tex(&cap, &left, &top, 1, &tex, &tex_w, &tex_h, &tex_x, &tex_y);
      if(tex)
      {
        cl_mem dev_tex = _yiyin_upload_tex(devid, tex, tex_w, tex_h);
        if(dev_tex)
        {
          err = _yiyin_cl_text_over(gd, devid, dev_out, dev_tex, tex_w, tex_h,
                                    tex_x, tex_y, cw, ch, d->film_caption_color, 1.0f);
          dt_opencl_release_mem_object(dev_tex);
        }
        dt_free_align(tex);
      }
      _yiyin_free_text_row(&cap);
    }
  }
  return err;
}


// the blur-mode render leg on device
static cl_int _yiyin_render_blur_cl(dt_iop_module_t *self,
                                    dt_dev_pixelpipe_iop_t *piece,
                                    cl_mem dev_in,
                                    cl_mem dev_out,
                                    const dt_iop_roi_t *const roi_in,
                                    const dt_iop_roi_t *const roi_out,
                                    const dt_iop_borders_data_t *d)
{
  dt_iop_borders_global_data_t *gd = self->global_data;
  const int devid = piece->pipe->devid;
  cl_int err = CL_SUCCESS;

  // the picture info rows: the planner sizes the fonts and measures the
  // texts, the render rasterizes them from the SAME plan so the reserved
  // canvas and the drawn bitmaps cannot disagree
  _yiyin_text_row_t rows[YIYIN_MAX_ROWS];
  for(int r = 0; r < YIYIN_MAX_ROWS; r++)
  {
    rows[r].coverage = NULL;
    rows[r].w = rows[r].h = 0;
  }
  // the plan and the layout describe the WHOLE picture (see the CPU leg)
  const int pic_w_full = piece->buf_in.width;
  const int pic_h_full = piece->buf_in.height;
  const float sc = roi_out->scale;
  int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
  _yiyin_rows_plan_t plan;
  _yiyin_plan_rows(&plan, pic_w_full, pic_h_full, d, &piece->pipe->image);
  const int n_rows = plan.n;
  // the layout takes the plan's OWN (unscaled) metrics -- scaling the
  // rendered bitmaps back down would compound rounding and drift the canvas
  for(int r = 0; r < n_rows; r++)
  {
    rows_w[r] = plan.w[r];
    rows_h[r] = plan.h[r];
  }
  _yiyin_layout_t L;
  _yiyin_layout(&L, pic_w_full, pic_h_full, d, rows_w, rows_h, n_rows);

  // the bitmaps are rasterised at the plane's scale
  if(n_rows)
  {
    for(int r = 0; r < n_rows; r++)
      _yiyin_render_text_row(plan.text[r], plan.font_px[r] * sc, d->text_font, &rows[r]);
  }
  _yiyin_free_rows_plan(&plan);
  const int cw = roi_out->width, ch = roi_out->height;

  // composite/SDF geometry, output-plane px
  const int pic_x = (int)roundf(L.main_left * sc) - roi_out->x;
  const int pic_y = (int)roundf(L.main_top * sc) - roi_out->y;
  const float sdf_cx = pic_x + roi_in->width * 0.5f;
  const float sdf_cy = pic_y + roi_in->height * 0.5f;
  const float sdf_hx = roi_in->width * 0.5f;
  const float sdf_hy = roi_in->height * 0.5f;
  const float sdf_radius = (float)(L.radius * sc);
  const float main_offset_x = (float)pic_x;
  const float main_offset_y = (float)pic_y;
  const float main_size_w = (float)roi_in->width;
  const float main_size_h = (float)roi_in->height;

  // device temporaries, declared ahead of any goto that skips them
  cl_mem dev_proxy = NULL, dev_proxy2 = NULL, dev_shadow = NULL;

  // 1. the blur proxy (device side, blur mode only)
  int pw = 0, ph = 0;
  const int wants_blur = d->backdrop == DT_IOP_BORDERS_BACKDROP_BLUR;
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
    const double sigma = L.shadow_blur * 0.5 * sc;
    const int margin = (int)ceil(3.0 * sigma + sdf_radius + 2.0);
    sub_x = MAX(0, pic_x - margin);
    sub_y = MAX(0, pic_y - margin);
    sub_w = MIN(cw, pic_x + roi_in->width + margin) - sub_x;
    sub_h = MIN(ch, pic_y + roi_in->height + margin) - sub_y;

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
     CLARG(d->color[0]), CLARG(d->color[1]), CLARG(d->color[2]),
     CLARG(overlay[0]), CLARG(overlay[1]), CLARG(overlay[2]), CLARG(overlay[3]),
     CLARG(main_offset_x), CLARG(main_offset_y),
     CLARG(main_size_w), CLARG(main_size_h),
     CLARG(sdf_cx), CLARG(sdf_cy), CLARG(sdf_hx), CLARG(sdf_hy),
     CLARG(sdf_radius),
     CLARG(cw), CLARG(ch),
     CLARG(pw), CLARG(ph),
     CLARG(sub_x), CLARG(sub_y), CLARG(sub_w), CLARG(sub_h));

  // 5. the picture info rows over the composited plane
  if(err == CL_SUCCESS && n_rows)
  {
    int row_left[YIYIN_MAX_ROWS], row_top[YIYIN_MAX_ROWS];
    const int canvas_w_s = (int)roundf(L.canvas_w * sc);
    const int canvas_h_s = (int)roundf(L.canvas_h * sc);
    _yiyin_stack_text_rows(rows, n_rows, canvas_w_s, canvas_h_s,
                           L.text_bottom * sc, row_left, row_top);
    for(int r = 0; r < n_rows; r++)
    {
      row_left[r] -= roi_out->x;
      row_top[r] -= roi_out->y;
    }
    float *tex = NULL;
    int tex_w = 0, tex_h = 0, tex_x = 0, tex_y = 0;
    _yiyin_pack_text_tex(rows, row_left, row_top, n_rows, &tex, &tex_w, &tex_h, &tex_x, &tex_y);
    if(tex)
    {
      cl_mem dev_tex = _yiyin_upload_tex(devid, tex, tex_w, tex_h);
      if(dev_tex)
      {
        const float white[3] = { 1.0f, 1.0f, 1.0f };
        const cl_int e = _yiyin_cl_text_over(gd, devid, dev_out, dev_tex, tex_w, tex_h,
                                             tex_x, tex_y, cw, ch, white,
                                             d->text_opacity / 100.0f);
        if(e != CL_SUCCESS) err = e;
        dt_opencl_release_mem_object(dev_tex);
      }
      dt_free_align(tex);
    }
  }
  for(int r = 0; r < 2; r++) _yiyin_free_text_row(&rows[r]);

finish:
  if(dev_proxy) dt_opencl_release_mem_object(dev_proxy);
  if(dev_proxy2) dt_opencl_release_mem_object(dev_proxy2);
  if(dev_shadow) dt_opencl_release_mem_object(dev_shadow);
  return err;
}


#endif


const char *name()
{
  return _("framing");
}

const char *aliases()
{
  return _("borders|enlarge canvas|expand canvas");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("add solid borders or margins around the image"),
                                      _("creative"),
                                      _("linear or non-linear, RGB, display-referred"),
                                      _("geometric, RGB"),
                                      _("linear or non-linear, RGB, display-referred"));
}


int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_DECORATION;
}

int flags()
{
  // no tiling: the blur leg samples a full-picture proxy and the film leg
  // spans the canvas, neither survives a tiled run
  return IOP_FLAGS_GUIDES_WIDGET | IOP_FLAGS_EXPAND_ROI_IN;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *const restrict points,
                           size_t points_count)
{
  dt_iop_borders_data_t *d = piece->data;

  if(d->mode != DT_IOP_BORDERS_MODE_BASIC)
  {
    // blur/film: the picture sits at its layout origin on the grown canvas
    if(_yiyin_is_identity(d)) return TRUE;
    _yiyin_layout_t L;
    if(d->mode == DT_IOP_BORDERS_MODE_FILM)
    {
      _yiyin_film_geom_t G;
      _yiyin_film_geom(&G, piece->buf_in.width, piece->buf_in.height, d);
      const float dx = 0.0f, dy = (float)G.band;
      float *const pts = DT_IS_ALIGNED(points);
      DT_OMP_FOR(if(points_count > 100))
      for(size_t i = 0; i < points_count * 2; i += 2)
      {
        pts[i] += dx;
        pts[i + 1] += dy;
      }
      return TRUE;
    }
    int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
    _yiyin_rows_plan_t plan;
    _yiyin_plan_rows(&plan, piece->buf_in.width, piece->buf_in.height, d, &piece->pipe->image);
    for(int r = 0; r < plan.n; r++) { rows_w[r] = plan.w[r]; rows_h[r] = plan.h[r]; }
    const int n_rows = plan.n;
    _yiyin_free_rows_plan(&plan);
    _yiyin_layout(&L, piece->buf_in.width, piece->buf_in.height, d, rows_w, rows_h, n_rows);
    const float dx = (float)L.main_left, dy = (float)L.main_top;
    float *const pts = DT_IS_ALIGNED(points);
    DT_OMP_FOR(if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      pts[i] += dx;
      pts[i + 1] += dy;
    }
    return TRUE;
  }

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);

  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] += border_size_l;
    pts[i + 1] += border_size_t;
  }

  return TRUE;
}

gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *const restrict points,
                               size_t points_count)
{
  dt_iop_borders_data_t *d = piece->data;

  if(d->mode != DT_IOP_BORDERS_MODE_BASIC)
  {
    if(_yiyin_is_identity(d)) return TRUE;
    _yiyin_layout_t L;
    float dx = 0.0f, dy = 0.0f;
    if(d->mode == DT_IOP_BORDERS_MODE_FILM)
    {
      _yiyin_film_geom_t G;
      _yiyin_film_geom(&G, piece->buf_in.width, piece->buf_in.height, d);
      dy = (float)G.band;
    }
    else
    {
      int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
      _yiyin_rows_plan_t plan;
      _yiyin_plan_rows(&plan, piece->buf_in.width, piece->buf_in.height, d, &piece->pipe->image);
      for(int r = 0; r < plan.n; r++) { rows_w[r] = plan.w[r]; rows_h[r] = plan.h[r]; }
      const int n_rows = plan.n;
      _yiyin_free_rows_plan(&plan);
      _yiyin_layout(&L, piece->buf_in.width, piece->buf_in.height, d, rows_w, rows_h, n_rows);
      dx = (float)L.main_left;
      dy = (float)L.main_top;
    }
    float *const pts = DT_IS_ALIGNED(points);
    DT_OMP_FOR(if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      pts[i] -= dx;
      pts[i + 1] -= dy;
    }
    return TRUE;
  }

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);
  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] -= border_size_l;
    pts[i + 1] -= border_size_t;
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
  dt_iop_borders_data_t *d = piece->data;

  if(d->mode != DT_IOP_BORDERS_MODE_BASIC)
  {
    dt_iop_image_fill(out, 0.0f, roi_out->width, roi_out->height, 1);
    int bx = 0, by = 0;
    if(d->mode == DT_IOP_BORDERS_MODE_FILM)
    {
      _yiyin_film_geom_t G;
      _yiyin_film_geom(&G, roi_in->width, roi_in->height, d);
      by = G.band;
    }
    else if(!_yiyin_is_identity(d))
    {
      _yiyin_layout_t L;
      int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
      _yiyin_rows_plan_t plan;
      _yiyin_plan_rows(&plan, roi_in->width, roi_in->height, d, &piece->pipe->image);
      for(int r = 0; r < plan.n; r++) { rows_w[r] = plan.w[r]; rows_h[r] = plan.h[r]; }
      const int n_rows = plan.n;
      _yiyin_free_rows_plan(&plan);
      _yiyin_layout(&L, roi_in->width, roi_in->height, d, rows_w, rows_h, n_rows);
      bx = CLAMP(L.main_left, 0, roi_out->width - roi_in->width);
      by = CLAMP(L.main_top, 0, roi_out->height - roi_in->height);
    }
    DT_OMP_FOR()
    for(int j = 0; j < roi_in->height; j++)
    {
      float *outb = out + (size_t)(j + by) * roi_out->width + bx;
      const float *inb = in + (size_t)j * roi_in->width;
      memcpy(outb, inb, sizeof(float) * roi_in->width);
    }
    return;
  }

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  const int border_in_x = binfo.border_in_x;
  const int border_in_y = binfo.border_in_y;

  // fill the image with 0 so that the added border isn't part of the mask
  dt_iop_image_fill(out, 0.0f, roi_out->width, roi_out->height, 1);

  // blit image inside border and fill the output with previous processed out
  DT_OMP_FOR()
  for(int j = 0; j < roi_in->height; j++)
  {
    float *outb = out + (size_t)(j + border_in_y) * roi_out->width + border_in_x;
    const float *inb = in + (size_t)j * roi_in->width;
    memcpy(outb, inb, sizeof(float) * roi_in->width);
  }
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_borders_data_t *d = piece->data;

  // NOTE: this callback runs ONLY from dt_dev_pixelpipe_get_dimensions(),
  // where roi_in is the whole unscaled image (x=y=0, scale=1). It defines
  // piece->buf_out, the canvas the rest of the pipeline works against, and
  // must therefore describe the FULL canvas -- never a viewport. The
  // viewport mapping lives in modify_roi_in.
  if(d->mode == DT_IOP_BORDERS_MODE_FILM)
  {
    if(d->film_band <= 0.0f) return;
    _yiyin_film_geom_t G;
    _yiyin_film_geom(&G, roi_in->width, roi_in->height, d);
    roi_out->height = MIN(roi_in->height + 2 * G.band, roi_in->height * 8);
    return;
  }

  if(d->mode == DT_IOP_BORDERS_MODE_BLUR)
  {
    if(_yiyin_is_identity(d)) return;
    int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
    _yiyin_rows_plan_t plan;
    _yiyin_plan_rows(&plan, roi_in->width, roi_in->height, d, &piece->pipe->image);
    for(int r = 0; r < plan.n; r++) { rows_w[r] = plan.w[r]; rows_h[r] = plan.h[r]; }
    const int n_rows = plan.n;
    _yiyin_free_rows_plan(&plan);
    _yiyin_layout_t L;
    _yiyin_layout(&L, roi_in->width, roi_in->height, d, rows_w, rows_h, n_rows);
    roi_out->width = MIN(MAX(L.canvas_w, roi_in->width), roi_in->width * 8);
    roi_out->height = MIN(MAX(L.canvas_h, roi_in->height), roi_in->height * 8);
    return;
  }

  const float size = fabsf(d->size);

  const gboolean is_constant_border = d->aspect == DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE;

  dt_iop_basis_t basis = d->basis;
  if(basis == DT_IOP_BORDERS_BASIS_AUTO)
  {
    // automatic/legacy/default behaviour:
    // for a constant border be sure to base the computation on the
    // larger border, failing that the border will have a different
    // size depending on the orientation.
    // for all other borders use the width.
    basis = is_constant_border
      ? DT_IOP_BORDERS_BASIS_LONGER : DT_IOP_BORDERS_BASIS_WIDTH;
  }
  if(basis == DT_IOP_BORDERS_BASIS_LONGER)
  {
    basis = roi_in->width > roi_in->height
      ? DT_IOP_BORDERS_BASIS_WIDTH : DT_IOP_BORDERS_BASIS_HEIGHT;
  }
  else if(basis == DT_IOP_BORDERS_BASIS_SHORTER)
  {
    basis = roi_in->width < roi_in->height
      ? DT_IOP_BORDERS_BASIS_WIDTH : DT_IOP_BORDERS_BASIS_HEIGHT;
  }

  assert(basis == DT_IOP_BORDERS_BASIS_WIDTH
         || basis == DT_IOP_BORDERS_BASIS_HEIGHT);

  const int *basis_in = NULL, *other_in = NULL;
  int *basis_out = NULL, *other_out = NULL;

  #define DT_IOP_BORDERS_ASSIGN(b, b_uc, o) \
    basis_in = &roi_in->b, basis_out = &roi_out->b, \
    other_in = &roi_in->o, other_out = &roi_out->o, \
    basis = DT_IOP_BORDERS_BASIS_ ## b_uc
  #define DT_IOP_BORDERS_ASSIGN_width DT_IOP_BORDERS_ASSIGN(width, WIDTH, height)
  #define DT_IOP_BORDERS_ASSIGN_height DT_IOP_BORDERS_ASSIGN(height, HEIGHT, width)
  #define DT_IOP_BORDERS_ASSIGN_BASIS(basis) DT_IOP_BORDERS_ASSIGN_ ## basis

  if(basis == DT_IOP_BORDERS_BASIS_WIDTH)
    DT_IOP_BORDERS_ASSIGN_BASIS(width);
  else if(basis == DT_IOP_BORDERS_BASIS_HEIGHT)
    DT_IOP_BORDERS_ASSIGN_BASIS(height);

  if(is_constant_border)
  {
    *basis_out = roundf((float)*basis_in / (1.0f - size));
    *other_out = *other_in + *basis_out - *basis_in;
  }
  else
  {
    const float image_aspect = (float)roi_in->width / (float)(roi_in->height);

    float aspect = (d->aspect == DT_IOP_BORDERS_ASPECT_IMAGE_VALUE)
      ? image_aspect
      : d->aspect;

    if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO)
      aspect = ((image_aspect < 1.0f && aspect > 1.0f)
                || (image_aspect > 1.0f && aspect < 1.0f))
        ? 1.0f / aspect
        : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE)
      aspect = (aspect < 1.0f) ? 1.0f / aspect : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT)
      aspect = (aspect > 1.0f) ? 1.0f / aspect : aspect;

    // first determine how large the border should be,
    float border_width = (float)*basis_in * (1.0f / (1.0f - size) - 1.0f);

    // then make sure we add that amount to the shorter side,
    if(basis == DT_IOP_BORDERS_BASIS_WIDTH && image_aspect < 1.0f)
      DT_IOP_BORDERS_ASSIGN_BASIS(height);
    else if(basis == DT_IOP_BORDERS_BASIS_HEIGHT && image_aspect > 1.0f)
      DT_IOP_BORDERS_ASSIGN_BASIS(width);

    // but add it to the longer side instead,
    // if the selected aspect ratio would cut off the image.
    if(basis == DT_IOP_BORDERS_BASIS_WIDTH && image_aspect < aspect)
      DT_IOP_BORDERS_ASSIGN_BASIS(height);
    else if(basis == DT_IOP_BORDERS_BASIS_HEIGHT && image_aspect > aspect)
      DT_IOP_BORDERS_ASSIGN_BASIS(width);

    if(basis == DT_IOP_BORDERS_BASIS_HEIGHT)
      aspect = 1.0f / aspect;

    *basis_out = roundf((float)*basis_in + border_width);
    *other_out = roundf((float)*basis_out / aspect);
  }

  // sanity check.
  const size_t max_dim = MAX(roi_in->width, roi_in->height);
  roi_out->width = CLAMP(roi_out->width, 1, 3 * max_dim);
  roi_out->height = CLAMP(roi_out->height, 1, 3 * max_dim);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_borders_data_t *d = piece->data;
  *roi_in = *roi_out;

  if(d->mode != DT_IOP_BORDERS_MODE_BASIC)
  {
    // buf_out (the canvas) and buf_in (the picture) are fixed by
    // modify_roi_out, so the picture's origin is a constant fraction of the
    // canvas and the input window is simply the canvas viewport shifted back
    // by it, clipped to the picture. Recomputing the layout here would make
    // it depend on the viewport instead of the picture.
    const int canvas_w = piece->buf_out.width;
    const int canvas_h = piece->buf_out.height;
    if(canvas_w <= 0 || canvas_h <= 0) return;

    // the origin is whatever the layout put on the full canvas -- it is not
    // simply centered (the bottom text slot shifts it up), so ask the same
    // layout the render uses
    int origin_x = 0, origin_y = 0;
    if(d->mode == DT_IOP_BORDERS_MODE_FILM)
    {
      _yiyin_film_geom_t G;
      _yiyin_film_geom(&G, piece->buf_in.width, piece->buf_in.height, d);
      origin_y = (int)roundf(G.band * roi_out->scale);
    }
    else if(!_yiyin_is_identity(d))
    {
      int rows_w[YIYIN_MAX_ROWS] = { 0, 0, 0 }, rows_h[YIYIN_MAX_ROWS] = { 0, 0, 0 };
      _yiyin_rows_plan_t plan;
      _yiyin_plan_rows(&plan, piece->buf_in.width, piece->buf_in.height, d,
                       &piece->pipe->image);
      for(int r = 0; r < plan.n; r++) { rows_w[r] = plan.w[r]; rows_h[r] = plan.h[r]; }
      const int n_rows = plan.n;
      _yiyin_free_rows_plan(&plan);
      _yiyin_layout_t L;
      _yiyin_layout(&L, piece->buf_in.width, piece->buf_in.height, d,
                    rows_w, rows_h, n_rows);
      origin_x = (int)roundf(L.main_left * roi_out->scale);
      origin_y = (int)roundf(L.main_top * roi_out->scale);
    }

    const float p_inw = (float)piece->buf_in.width * roi_out->scale;
    const float p_inh = (float)piece->buf_in.height * roi_out->scale;

    // clip the shifted window to the picture's extent
    const int x0 = MAX(0, roi_out->x - origin_x);
    const int y0 = MAX(0, roi_out->y - origin_y);
    const int x1 = MIN((int)roundf(p_inw), roi_out->x - origin_x + roi_out->width);
    const int y1 = MIN((int)roundf(p_inh), roi_out->y - origin_y + roi_out->height);

    roi_in->x = x0;
    roi_in->y = y0;
    roi_in->width = MAX(1, x1 - x0);
    roi_in->height = MAX(1, y1 - y0);
    return;
  }

  const float bw = (piece->buf_out.width - piece->buf_in.width) * roi_out->scale;
  const float bh = (piece->buf_out.height - piece->buf_in.height) * roi_out->scale;

  // don't request outside image (no px for borders)
  roi_in->x = MAX(roundf(roi_out->x - bw * d->pos_h), 0);
  roi_in->y = MAX(roundf(roi_out->y - bh * d->pos_v), 0);

  // subtract upper left border from dimensions
  roi_in->width -= MAX(roundf(bw * d->pos_h - roi_out->x), 0);
  roi_in->height -= MAX(roundf(bh * d->pos_v - roi_out->y), 0);

  // subtract lower right border from dimensions
  const float p_inw = (float)piece->buf_in.width * roi_out->scale;
  const float p_inh = (float)piece->buf_in.height * roi_out->scale;

  roi_in->width  -= MAX(roundf((float)(roi_in->x + roi_in->width) - p_inw), 0);
  roi_in->height -= MAX(roundf((float)(roi_in->y + roi_in->height) - p_inh), 0);

  // sanity check: don't request nothing or outside roi
  roi_in->width = MIN(p_inw, MAX(1, roi_in->width));
  roi_in->height = MIN(p_inh, MAX(1, roi_in->height));

  // FIXME: clamping to 1 leads to a one-pixel visual glitch if the
  // right/bottom border completely fills the viewport, but
  // changing it to 0 breaks all of the tiling_callback functions with
  // a division by zero.
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_borders_data_t *const d = piece->data;
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(d->mode == DT_IOP_BORDERS_MODE_FILM)
  {
    if(d->film_band <= 0.0f)
    {
      dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
      return;
    }
    _yiyin_process_film(in, out, roi_in, roi_out, d);
    return;
  }

  if(d->mode == DT_IOP_BORDERS_MODE_BLUR)
  {
    if(_yiyin_is_identity(d))
    {
      if(roi_out->width == roi_in->width && roi_out->height == roi_in->height)
        memcpy(out, in, sizeof(float) * (size_t)roi_out->width * roi_out->height * 4);
      else
        dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
      return;
    }
    _yiyin_render_blur(self, piece, ivoid, ovoid, roi_in, roi_out, d);
    return;
  }

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  dt_iop_copy_image_with_border((float*)ovoid, (const float*)ivoid, &binfo);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_borders_data_t *d = piece->data;
  dt_iop_borders_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  if(d->mode == DT_IOP_BORDERS_MODE_FILM)
  {
    if(d->film_band <= 0.0f)
    {
      const size_t region[3] = { roi_in->width, roi_in->height, 1 };
      return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN,
                                          region);
    }
    return _yiyin_process_film_cl(gd, devid, dev_in, dev_out, roi_in, roi_out, d);
  }

  if(d->mode == DT_IOP_BORDERS_MODE_BLUR)
  {
    if(_yiyin_is_identity(d))
    {
      const size_t region[3] = { roi_in->width, roi_in->height, 1 };
      return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN,
                                          region);
    }
    return _yiyin_render_blur_cl(self, piece, dev_in, dev_out, roi_in, roi_out, d);
  }

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  const int width = roi_out->width;
  const int height = roi_out->height;

  // ----- Filling border
  const float col[4] = { d->color[0], d->color[1], d->color[2], 1.0f };
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                            CLARG(dev_out), CLARGINT(0), CLARGINT(0),
                            CLARG(width), CLARG(height), CLARG(col));
  if(err != CL_SUCCESS) goto error;

  if(binfo.frame_size != 0)
  {
    const float col_frame[4] = { d->frame_color[0],
                                 d->frame_color[1],
                                 d->frame_color[2], 1.0f };

    const int roi_frame_in_width   = binfo.frame_br_in_x - binfo.frame_tl_in_x;
    const int roi_frame_in_height  = binfo.frame_br_in_y - binfo.frame_tl_in_y;
    const int roi_frame_out_width  = binfo.frame_br_out_x - binfo.frame_tl_out_x;
    const int roi_frame_out_height = binfo.frame_br_out_y - binfo.frame_tl_out_y;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                              CLARG(dev_out),
                              CLARG(binfo.frame_tl_out_x), CLARG(binfo.frame_tl_out_y),
                              CLARG(roi_frame_out_width), CLARG(roi_frame_out_height),
                              CLARG(col_frame));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                              CLARG(dev_out),
                              CLARG(binfo.frame_tl_in_x), CLARG(binfo.frame_tl_in_y),
                              CLARG(roi_frame_in_width), CLARG(roi_frame_in_height),
                              CLARG(col));
    if(err != CL_SUCCESS) goto error;
  }

  const size_t oorigin[2] = { binfo.border_in_x, binfo.border_in_y };
  const size_t region[2]  = { roi_in->width, roi_in->height };

  // copy original input from dev_in -> dev_out as starting point
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, oorigin, region);

error:
  return err;
}
#endif


void init_global(dt_iop_module_so_t *self)
{
  const int program = 2;  // basic.cl from programs.conf
  const int program_f = 53; // yiyin.cl from programs.conf (blur/film legs)

  dt_iop_borders_global_data_t *gd = malloc(sizeof(dt_iop_borders_global_data_t));
  self->data = gd;
  gd->kernel_borders_fill = dt_opencl_create_kernel(program, "borders_fill");
  gd->kernel_yiyin_composite = dt_opencl_create_kernel(program_f, "yiyin_composite");
  gd->kernel_yiyin_shadow_sdf = dt_opencl_create_kernel(program_f, "yiyin_shadow_sdf");
  gd->kernel_yiyin_downsample = dt_opencl_create_kernel(program_f, "yiyin_downsample");
  gd->kernel_yiyin_film_composite = dt_opencl_create_kernel(program_f, "yiyin_film_composite");
  gd->kernel_yiyin_text_over = dt_opencl_create_kernel(program_f, "yiyin_text_over");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_borders_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_borders_fill);
  dt_opencl_free_kernel(gd->kernel_yiyin_composite);
  dt_opencl_free_kernel(gd->kernel_yiyin_shadow_sdf);
  dt_opencl_free_kernel(gd->kernel_yiyin_downsample);
  dt_opencl_free_kernel(gd->kernel_yiyin_film_composite);
  dt_opencl_free_kernel(gd->kernel_yiyin_text_over);
  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)p1;
  dt_iop_borders_data_t *d = piece->data;
  memcpy(d, p, sizeof(dt_iop_borders_params_t));
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_borders_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  // the introspection machinery zeroes char arrays; fill the pattern/text
  // defaults by hand
  dt_iop_borders_params_t *d = self->default_params;
  g_strlcpy(d->text_model, "{Make} {Model}", sizeof(d->text_model));
  g_strlcpy(d->text_lens, "{Lens}", sizeof(d->text_lens));
  g_strlcpy(d->text_params, "{FocalLengthIn35mmFormat}mm f/{FNumber} {ExposureTime}s ISO{ISO}",
            sizeof(d->text_params));
  g_strlcpy(d->text_font, "Sans Bold", sizeof(d->text_font));
  g_strlcpy(d->film_caption, "KODAK PORTRA 400", sizeof(d->film_caption));
  g_strlcpy(d->film_font, "Sans Bold", sizeof(d->film_font));
  d->film_caption_color[0] = d->film_caption_color[1] = d->film_caption_color[2] = 1.0f;
  d->text_size1 = 1.8f;
  d->text_size2 = 1.8f;
  d->text_size3 = 2.2f;
  d->text_opacity = 90.0f;
  d->blur_amount = 10.0f;
  d->mini_top_bottom_margin = 10.0f;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_borders_params_t p
    = { .color = { 1.0f, 1.0f, 1.0f },
        .aspect = 3.0f / 2.0f,
        .aspect_orient = 0,
        .size = 0.1f,
        .pos_h = 0.5f,
        .pos_v = 0.5f,
        .frame_size = 0.0f,
        .frame_offset = 0.5f,
        .frame_color = { 0.0f, 0.0f, 0.0f },
        .max_border_size = TRUE,
        .basis = DT_IOP_BORDERS_BASIS_AUTO,
        .mode = DT_IOP_BORDERS_MODE_BASIC,
        .backdrop = DT_IOP_BORDERS_BACKDROP_BLUR,
        .blur_amount = 10.0f,
        .adaptive_backdrop = 1,
        .main_image_width_rate = 90.0f,
        .mini_top_bottom_margin = 10.0f,
        .aspect_w = 0,
        .aspect_h = 0,
        .landscape_output = 0,
        .corner_radius = 2.1f,
        .shadow = 6.0f,
        .text_bottom_offset = 0.027f,
        .text_show = TRUE,
        .text_model = "{Make} {Model}",
        .text_params = "{FocalLengthIn35mmFormat}mm f/{FNumber} {ExposureTime}s ISO{ISO}",
        .text_size = 2.2f,
        .film_band = 12.0f,
        .film_caption = "KODAK PORTRA 400",
        .film_caption_size = 50.0f,
        .text_rows = 3,
        .text_lens = "{Lens}",
        .text_font = "Sans Bold",
        .text_size1 = 1.8f,
        .text_size2 = 1.8f,
        .text_size3 = 2.2f,
        .text_opacity = 90.0f,
        .film_font = "Sans Bold",
        .film_caption_color = { 1.0f, 1.0f, 1.0f } };
  g_strlcpy(p.aspect_text, "3:2", sizeof(p.aspect_text));
  g_strlcpy(p.pos_h_text, "1/2", sizeof(p.pos_h_text));
  g_strlcpy(p.pos_v_text, "1/2", sizeof(p.pos_v_text));
  dt_gui_presets_add_generic(_("15:10 postcard white"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_NONE);

  p.color[0] = p.color[1] = p.color[2] = 0.0f;
  p.frame_color[0] = p.frame_color[1] = p.frame_color[2] = 1.0f;
  dt_gui_presets_add_generic(_("15:10 postcard black"), self->op,
                             self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_NONE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  if(p->mode != DT_IOP_BORDERS_MODE_BASIC) return;


  if(fabsf(p->color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  if(fabsf(p->frame_color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->frame_color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->frame_color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  GdkRGBA c = (GdkRGBA){.red = self->picked_color[0],
                        .green = self->picked_color[1],
                        .blue = self->picked_color[2],
                        .alpha = 1.0 };

  if(picker == g->frame_picker)
  {
    p->frame_color[0] = self->picked_color[0];
    p->frame_color[1] = self->picked_color[1];
    p->frame_color[2] = self->picked_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &c);
  }
  else if(picker == g->border_picker)
  {
    p->color[0] = self->picked_color[0];
    p->color[1] = self->picked_color[1];
    p->color[2] = self->picked_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _aspect_changed(GtkWidget *combo,
                            dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_ASPECT_COUNT)
  {
    p->aspect = _aspect_ratios[which];
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->aspect_slider,p->aspect);
    DT_LEAVE_GUI_UPDATE();
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _position_h_changed(GtkWidget *combo,
                                dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    p->pos_h = _pos_h_ratios[which];
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->pos_h_slider,p->pos_h);
    DT_LEAVE_GUI_UPDATE();
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _position_v_changed(GtkWidget *combo,
                                dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_POSITION_V_COUNT)
  {
    p->pos_v = _pos_v_ratios[which];
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->pos_v_slider,p->pos_v);
    DT_LEAVE_GUI_UPDATE();
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// show/hide per mode: basic keeps the legacy widgets, blur shows the yiyin
// backdrop set (the fill comes from the border color row), film shows the
// strip controls
static void _mode_showhide(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  const gboolean basic = p->mode == DT_IOP_BORDERS_MODE_BASIC;
  const gboolean blur = p->mode == DT_IOP_BORDERS_MODE_BLUR;
  GtkWidget *const basic_widgets[] = { g->basis, g->size, g->aspect, g->aspect_slider,
                                       g->aspect_orient, g->pos_h, g->pos_h_slider,
                                       g->pos_v, g->pos_v_slider, g->frame_size,
                                       g->frame_offset, g->frame_color_box };
  for(int i = 0; i < (int)G_N_ELEMENTS(basic_widgets); i++)
    gtk_widget_set_visible(basic_widgets[i], basic);
  // the border color row doubles as the blur backdrop fill color
  gtk_widget_set_visible(g->colorpick_box, basic || blur);
  gtk_widget_set_visible(g->border_picker, basic);

  gtk_widget_set_visible(g->backdrop, blur);
  gtk_widget_set_visible(g->blur_amount, blur);
  gtk_widget_set_visible(g->adaptive_backdrop, blur);
  gtk_widget_set_visible(g->main_image_width_rate, blur);
  gtk_widget_set_visible(g->mini_top_bottom_margin, blur);
  gtk_widget_set_visible(g->aspect_w, blur);
  gtk_widget_set_visible(g->aspect_h, blur);
  gtk_widget_set_visible(g->landscape_output, blur);
  gtk_widget_set_visible(g->corner_radius, blur);
  gtk_widget_set_visible(g->shadow, blur);
  gtk_widget_set_visible(g->text_show, blur);
  const gboolean rows = blur && p->text_show;
  gtk_widget_set_visible(g->text_rows, rows);
  gtk_widget_set_visible(g->text_model, rows);
  gtk_widget_set_visible(g->text_lens, rows && p->text_rows >= 2);
  gtk_widget_set_visible(g->text_params, rows && p->text_rows >= 3);
  gtk_widget_set_visible(g->text_font, rows);
  gtk_widget_set_visible(g->text_size1, rows && p->text_rows >= 1);
  gtk_widget_set_visible(g->text_size2, rows && p->text_rows >= 2);
  gtk_widget_set_visible(g->text_size3, rows && p->text_rows >= 3);
  gtk_widget_set_visible(g->text_opacity, rows);

  const gboolean film = p->mode == DT_IOP_BORDERS_MODE_FILM;
  gtk_widget_set_visible(g->film_band, film);
  gtk_widget_set_visible(g->film_caption, film);
  gtk_widget_set_visible(g->film_font, film);
  gtk_widget_set_visible(g->film_color_box, film);
  gtk_widget_set_visible(g->film_caption_size, film);
}

static void _mode_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_borders_params_t *p = self->params;
  p->mode = dt_bauhaus_combobox_get(widget);
  _mode_showhide(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _showhide_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  _mode_showhide(self);
}

// commit a pattern entry on Enter (or focus leave); per-keystroke history
// items would flood the stack
static void _text_entry_activate(GtkEntry *entry, dt_iop_module_t *self)
{
  dt_iop_borders_params_t *p = self->params;
  dt_iop_borders_gui_data_t *g = self->gui_data;
  const char *t = gtk_entry_get_text(entry);
  if(entry == GTK_ENTRY(g->text_model))
    g_strlcpy(p->text_model, t ? t : "", sizeof(p->text_model));
  else if(entry == GTK_ENTRY(g->text_lens))
    g_strlcpy(p->text_lens, t ? t : "", sizeof(p->text_lens));
  else if(entry == GTK_ENTRY(g->text_params))
    g_strlcpy(p->text_params, t ? t : "", sizeof(p->text_params));
  else if(entry == GTK_ENTRY(g->text_font))
    g_strlcpy(p->text_font, t ? t : "", sizeof(p->text_font));
  else if(entry == GTK_ENTRY(g->film_caption))
    g_strlcpy(p->film_caption, t ? t : "", sizeof(p->film_caption));
  else if(entry == GTK_ENTRY(g->film_font))
    g_strlcpy(p->film_font, t ? t : "", sizeof(p->film_font));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean _text_entry_focus_out(GtkWidget *widget, GdkEventFocus *event,
                                      dt_iop_module_t *self)
{
  _text_entry_activate(GTK_ENTRY(widget), self);
  return FALSE;
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  if(p->mode != DT_IOP_BORDERS_MODE_BASIC)
  {
    _mode_showhide(self);
    return;
  }

  int k;
  if(!w || w == g->aspect_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_ASPECT_COUNT; k++)
    {
      if(fabsf(p->aspect - _aspect_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->aspect, k);
  }
  if(!w || w == g->pos_h_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_H_COUNT; k++)
    {
      if(fabsf(p->pos_h - _pos_h_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_h, k);
  }
  if(!w || w == g->pos_v_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_V_COUNT; k++)
    {
      if(fabsf(p->pos_v - _pos_v_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_v, k);
  }

  const char *reason = NULL;
  if(p->size <= 0.0f)
    reason = _("increase border size to enable the frame line");
  else if(p->pos_h <= 0.0f)
    reason = _("increase horizontal offset to enable the frame line");
  else if(p->pos_h >= 1.0f)
    reason = _("decrease horizontal offset to enable the frame line");
  else if(p->pos_v <= 0.0f)
    reason = _("increase vertical offset to enable the frame line");
  else if(p->pos_v >= 1.0f)
    reason = _("decrease vertical offset to enable the frame line");

  const gboolean enable_frameline = (reason == NULL);

  gtk_widget_set_sensitive(g->frame_size, enable_frameline);
  gtk_widget_set_sensitive(g->frame_offset, enable_frameline);
  gtk_widget_set_sensitive(g->frame_color_box, enable_frameline);

  if(enable_frameline)
  {
    gtk_widget_set_tooltip_text(g->frame_size,
                                _("size of the frame line in percent of min border width"));
    gtk_widget_set_tooltip_text(g->frame_offset,
                                _("offset of the frame line beginning on image side"));
    gtk_widget_set_tooltip_text(g->frame_colorpick,
                                _("select frame line color"));
    gtk_widget_set_tooltip_text(g->frame_picker,
                                _("pick frame line color from image"));
  }
  else
  {
    gtk_widget_set_tooltip_text(g->frame_size, reason);
    gtk_widget_set_tooltip_text(g->frame_offset, reason);
    gtk_widget_set_tooltip_text(g->frame_colorpick, reason);
    gtk_widget_set_tooltip_text(g->frame_picker, reason);
  }
}

static void _colorpick_color_set(GtkColorButton *widget,
                                dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_borders_params_t *p = self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _frame_colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_borders_params_t *p = self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->frame_color[0] = c.red;
  p->frame_color[1] = c.green;
  p->frame_color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _film_colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_borders_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->film_caption_color[0] = c.red;
  p->film_caption_color[1] = c.green;
  p->film_caption_color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  gui_changed(self, NULL, NULL);

  // ----- Border Color
  GdkRGBA c = (GdkRGBA){.red   = p->color[0],
                        .green = p->color[1],
                        .blue  = p->color[2],
                        .alpha = 1.0 };

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);

  // ----- Frame Color
  GdkRGBA fc = (GdkRGBA){
    .red   = p->frame_color[0],
    .green = p->frame_color[1],
    .blue  = p->frame_color[2],
    .alpha = 1.0
  };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &fc);

  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_combobox_set(g->backdrop, p->backdrop);
  dt_bauhaus_slider_set(g->blur_amount, p->blur_amount);
  dt_bauhaus_toggle_set(g->adaptive_backdrop, p->adaptive_backdrop);
  dt_bauhaus_slider_set(g->main_image_width_rate, p->main_image_width_rate);
  dt_bauhaus_slider_set(g->mini_top_bottom_margin, p->mini_top_bottom_margin);
  dt_bauhaus_slider_set(g->aspect_w, p->aspect_w);
  dt_bauhaus_slider_set(g->aspect_h, p->aspect_h);
  dt_bauhaus_toggle_set(g->landscape_output, p->landscape_output);
  dt_bauhaus_slider_set(g->corner_radius, p->corner_radius);
  dt_bauhaus_slider_set(g->shadow, p->shadow);
  dt_bauhaus_toggle_set(g->text_show, p->text_show);
  gtk_entry_set_text(GTK_ENTRY(g->text_model), p->text_model);
  gtk_entry_set_text(GTK_ENTRY(g->text_lens), p->text_lens);
  gtk_entry_set_text(GTK_ENTRY(g->text_params), p->text_params);
  dt_bauhaus_slider_set(g->text_rows, p->text_rows);
  gtk_entry_set_text(GTK_ENTRY(g->text_font), p->text_font);
  dt_bauhaus_slider_set(g->text_size1, p->text_size1);
  dt_bauhaus_slider_set(g->text_size2, p->text_size2);
  dt_bauhaus_slider_set(g->text_size3, p->text_size3);
  dt_bauhaus_slider_set(g->text_opacity, p->text_opacity);
  dt_bauhaus_slider_set(g->film_band, p->film_band);
  gtk_entry_set_text(GTK_ENTRY(g->film_caption), p->film_caption);
  gtk_entry_set_text(GTK_ENTRY(g->film_font), p->film_font);
  GdkRGBA filmc = (GdkRGBA){.red   = p->film_caption_color[0],
                            .green = p->film_caption_color[1],
                            .blue  = p->film_caption_color[2],
                            .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->film_colorpick), &filmc);
  dt_bauhaus_slider_set(g->film_caption_size, p->film_caption_size);
  _mode_showhide(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = IOP_GUI_ALLOC(borders);
  dt_iop_borders_params_t *p = self->params;
  dt_iop_borders_params_t *dp = self->default_params;

  // ── the frame modes ──
  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("basic border, yiyin backdrop, or a film strip"));
  g_signal_connect_after(G_OBJECT(g->mode), "value-changed", G_CALLBACK(_mode_changed), self);

  g->backdrop = dt_bauhaus_combobox_from_params(self, "backdrop");
  gtk_widget_set_tooltip_text(g->backdrop, _("fill the backdrop band with a solid color or a blurred copy of the picture"));
  g_signal_connect_after(G_OBJECT(g->backdrop), "value-changed",
                         G_CALLBACK(_mode_changed), self);

  g->blur_amount = dt_bauhaus_slider_from_params(self, "blur_amount");
  dt_bauhaus_slider_set_format(g->blur_amount, "%");
  gtk_widget_set_tooltip_text(g->blur_amount, _("how strongly the blurred backdrop is blurred"));

  g->adaptive_backdrop = dt_bauhaus_toggle_from_params(self, "adaptive_backdrop");
  gtk_widget_set_tooltip_text(g->adaptive_backdrop, _("dim a bright blurred backdrop so the picture stands out"));
  g_signal_connect_after(G_OBJECT(g->adaptive_backdrop), "value-changed",
                         G_CALLBACK(_showhide_changed), self);

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

  g->text_show = dt_bauhaus_toggle_from_params(self, "text_show");
  gtk_widget_set_tooltip_text(g->text_show, _("print the picture info below the picture"));
  g_signal_connect_after(G_OBJECT(g->text_show), "value-changed",
                         G_CALLBACK(_showhide_changed), self);
  g->text_rows = dt_bauhaus_slider_from_params(self, "text_rows");
  dt_bauhaus_slider_set_step(g->text_rows, 1.0);
  dt_bauhaus_slider_set_digits(g->text_rows, 0);
  gtk_widget_set_tooltip_text(g->text_rows, _("how many of the three info rows to print"));
  g_signal_connect_after(G_OBJECT(g->text_rows), "value-changed",
                         G_CALLBACK(_showhide_changed), self);

  g->text_model = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->text_model), "{Make} {Model}");
  gtk_widget_set_tooltip_text(g->text_model, _("the first info row; placeholders: {Make} {Model} {Lens} {FocalLength} {FocalLengthIn35mmFormat} {FNumber} {ExposureTime} {ISO} -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->text_model), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->text_model), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->text_model);

  g->text_lens = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->text_lens), "{Lens}");
  gtk_widget_set_tooltip_text(g->text_lens, _("info row 2, the lens -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->text_lens), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->text_lens), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->text_lens);

  g->text_params = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->text_params),
                                 "{FocalLengthIn35mmFormat}mm f/{FNumber} {ExposureTime}s ISO{ISO}");
  gtk_widget_set_tooltip_text(g->text_params, _("the second info row, same placeholders -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->text_params), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->text_params), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->text_params);

  g->text_size1 = dt_bauhaus_slider_from_params(self, "text_size1");
  dt_bauhaus_slider_set_format(g->text_size1, "%");
  gtk_widget_set_tooltip_text(g->text_size1, _("info row 1 (make and model) size as a share of the canvas width"));

  g->text_size2 = dt_bauhaus_slider_from_params(self, "text_size2");
  dt_bauhaus_slider_set_format(g->text_size2, "%");
  gtk_widget_set_tooltip_text(g->text_size2, _("info row 2 (lens) size as a share of the canvas width"));

  g->text_size3 = dt_bauhaus_slider_from_params(self, "text_size3");
  dt_bauhaus_slider_set_format(g->text_size3, "%");
  gtk_widget_set_tooltip_text(g->text_size3, _("info row 3 (exposure) size as a share of the canvas width"));

  g->text_opacity = dt_bauhaus_slider_from_params(self, "text_opacity");
  dt_bauhaus_slider_set_format(g->text_opacity, "%");
  gtk_widget_set_tooltip_text(g->text_opacity, _("the opacity of all three info rows"));

  g->text_font = gtk_entry_new_with_buffer(gtk_entry_buffer_new("", -1));
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->text_font), "Sans Bold");
  gtk_widget_set_tooltip_text(g->text_font, _("the font for all three info rows, a Pango font description -- try \"Heiti SC\" (黑体), \"Kaiti SC\" (楷体), \"Songti SC\" (宋体), \"PingFang SC\" -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->text_font), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->text_font), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->text_font);

  g->film_band = dt_bauhaus_slider_from_params(self, "film_band");
  dt_bauhaus_slider_set_format(g->film_band, "%");
  gtk_widget_set_tooltip_text(g->film_band, _("the black band width above and below the picture"));

  g->film_caption = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->film_caption), _("film edge marking"));
  gtk_widget_set_tooltip_text(g->film_caption, _("the film edge marking text in the bottom band -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->film_caption), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->film_caption), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->film_caption);

  g->film_font = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->film_font), "Sans Bold");
  gtk_widget_set_tooltip_text(g->film_font, _("the edge marking font, a Pango font description -- commit with Enter"));
  g_signal_connect(G_OBJECT(g->film_font), "activate", G_CALLBACK(_text_entry_activate), self);
  g_signal_connect(G_OBJECT(g->film_font), "focus-out-event",
                   G_CALLBACK(_text_entry_focus_out), self);
  dt_gui_box_add(self->widget, g->film_font);

  GtkWidget *film_label = dtgtk_reset_label_new(_("film text color"), self,
                                                &p->film_caption_color, 3 * sizeof(float));
  GdkRGBA filmcol = (GdkRGBA){.red   = dp->film_caption_color[0],
                              .green = dp->film_caption_color[1],
                              .blue  = dp->film_caption_color[2],
                              .alpha = 1.0 };
  g->film_colorpick = gtk_color_button_new_with_rgba(&filmcol);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->film_colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->film_colorpick), _("select film text color"));
  gtk_widget_set_tooltip_text(g->film_colorpick, _("the film edge marking color"));
  g_signal_connect(G_OBJECT(g->film_colorpick), "color-set",
                   G_CALLBACK(_film_colorpick_color_set), self);
  g->film_color_box = dt_gui_hbox(dt_gui_expand(film_label), g->film_colorpick);
  dt_gui_box_add(self->widget, g->film_color_box);

  g->film_caption_size = dt_bauhaus_slider_from_params(self, "film_caption_size");
  dt_bauhaus_slider_set_format(g->film_caption_size, "%");
  gtk_widget_set_tooltip_text(g->film_caption_size, _("the edge marking size as a share of the band width"));

  g->basis = dt_bauhaus_combobox_from_params(self, "basis");
  gtk_widget_set_tooltip_text(g->basis,
                              _("which dimension to use for the size calculation"));

  g->size = dt_bauhaus_slider_from_params(self, "size");
  dt_bauhaus_slider_set_digits(g->size, 4);
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size,
                              _("size of the border in percent of the chosen basis"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->aspect, self, NULL, N_("aspect"),
                               _("select the aspect ratio\n"
                                 "(right-click on slider below to type your own w:h)"),
                               0, _aspect_changed, self,
                               N_("image"),
                               N_("3:1"),
                               N_("95:33"),
                               N_("CinemaScope 2.39:1"),
                               N_("2:1"),
                               N_("16:9"),
                               N_("5:3"),
                               N_("US Legal 8.5x14"),
                               N_("golden cut"),
                               N_("16:10"),
                               N_("3:2 (4x6, 10x15cm)"),
                               N_("A4"),
                               N_("DIN"),
                               N_("7:5"),
                               N_("4:3"),
                               N_("US Letter 8.5x11"),
                               N_("14:11"),
                               N_("5:4 (8x10)"),
                               N_("square"),
                               N_("constant border"),
                               N_("custom..."));
  dt_gui_box_add(self->widget, g->aspect);

  g->aspect_slider = dt_bauhaus_slider_from_params(self, "aspect");
  gtk_widget_set_tooltip_text(g->aspect_slider, _("set the custom aspect ratio\n"
                                                  "(right-click to enter number or w:h)"));

  g->aspect_orient = dt_bauhaus_combobox_from_params(self, "aspect_orient");
  gtk_widget_set_tooltip_text(g->aspect_orient,
                              _("aspect ratio orientation of the image with border"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_h, self, NULL, N_("horizontal position"),
                               _("select the horizontal position ratio relative to top\n"
                                 "(right-click on slider below to type your own x:w)"),
                               0, _position_h_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"),
                               N_("5/8"), N_("2/3"), N_("custom..."));
  dt_gui_box_add(self->widget, g->pos_h);

  g->pos_h_slider = dt_bauhaus_slider_from_params(self, "pos_h");
  gtk_widget_set_tooltip_text(g->pos_h_slider, _("custom horizontal position"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_v, self, NULL, N_("vertical position"),
                               _("select the vertical position ratio relative to left\n"
                                 "(right-click on slider below to type your own y:h)"),
                               0, _position_v_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"),
                               N_("5/8"), N_("2/3"), N_("custom..."));
  dt_gui_box_add(self->widget, g->pos_v);

  g->pos_v_slider = dt_bauhaus_slider_from_params(self, "pos_v");
  gtk_widget_set_tooltip_text(g->pos_v_slider, _("custom vertical position"));

  g->frame_size = dt_bauhaus_slider_from_params(self, "frame_size");
  dt_bauhaus_slider_set_digits(g->frame_size, 4);
  dt_bauhaus_slider_set_format(g->frame_size, "%");
  gtk_widget_set_tooltip_text(g->frame_size,
                              _("size of the frame line in percent of min border width"));

  g->frame_offset = dt_bauhaus_slider_from_params(self, "frame_offset");
  dt_bauhaus_slider_set_digits(g->frame_offset, 4);
  dt_bauhaus_slider_set_format(g->frame_offset, "%");
  gtk_widget_set_tooltip_text(g->frame_offset,
                              _("offset of the frame line beginning on image side"));

  GdkRGBA color = (GdkRGBA){.red   = dp->color[0],
                            .green = dp->color[1],
                            .blue  = dp->color[2],
                            .alpha = 1.0 };

  GdkRGBA frame_color = (GdkRGBA){.red = dp->frame_color[0],
                                  .green = dp->frame_color[1],
                                  .blue = dp->frame_color[2],
                                  .alpha = 1.0 };

  GtkWidget *label = dtgtk_reset_label_new(_("border color"), self, &p->color, 3 * sizeof(float));
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select border color"));
  gtk_widget_set_tooltip_text(g->colorpick, _("the border color, or the blur-mode backdrop fill"));
  g_signal_connect(G_OBJECT(g->colorpick), "color-set",
                   G_CALLBACK(_colorpick_color_set), self);
  g->border_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->border_picker),
                              _("pick border color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("border color"),
                       g->border_picker, &dt_action_def_color_picker);
  g->colorpick_box = dt_gui_hbox(dt_gui_expand(label), g->colorpick, g->border_picker);
  dt_gui_box_add(self->widget, g->colorpick_box);

  label = dtgtk_reset_label_new(_("frame line color"), self, &p->frame_color, 3 * sizeof(float));
  g->frame_colorpick = gtk_color_button_new_with_rgba(&frame_color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->frame_colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->frame_colorpick),
                             _("select frame line color"));
  g_signal_connect(G_OBJECT(g->frame_colorpick), "color-set",
                   G_CALLBACK(_frame_colorpick_color_set), self);
  g->frame_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->frame_picker),
                              _("pick frame line color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("frame line color"),
                       g->frame_picker, &dt_action_def_color_picker);
  g->frame_color_box = dt_gui_hbox(dt_gui_expand(label), g->frame_colorpick, g->frame_picker);
  dt_gui_box_add(self->widget, g->frame_color_box);

  _mode_showhide(self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
