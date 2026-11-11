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

// yiyin frame render legs: the composite (backdrop band + optional dim
// overlay + shadow + the 1:1 picture clipped by the rounded-rect SDF), the
// shadow SDF stamp and the box-average downsample (blur proxy + probe).
// Coordinate conventions: everything is in THIS RUN's plane pixels -- the
// module passes output-plane-relative geometry; the picture lookup is
// EXACT 1:1 (yiyin never rescales the picture -- it grows the canvas
// instead): output pixel p samples the input at p - mainOffset.

constant sampler_t yiyin_sampler
    = CLK_NORMALIZED_COORDS_TRUE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

static float yiyin_sd_rounded_rect(float2 p, float2 center, float2 half_ext, float radius)
{
  float2 q = fabs(p - center) - half_ext + (float2)(radius);
  float2 o = fmax(q, (float2)(0.0f));
  return length(o) + fmin(fmax(q.x, q.y), 0.0f) - radius;
}

// inside coverage with a 1px antialias band
static float yiyin_rect_coverage(float2 p, float2 center, float2 half_ext, float radius)
{
  float sd = yiyin_sd_rounded_rect(p, center, half_ext, radius);
  return 1.0f - smoothstep(-0.5f, 0.5f, sd);
}

kernel void
yiyin_shadow_sdf(write_only image2d_t out,
                 const int width,
                 const int height,
                 const float2 center,
                 const float2 half_ext,
                 const float radius,
                 const int sub_x,
                 const int sub_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  float2 p = (float2)(sub_x + x, sub_y + y) + 0.5f;
  float a = yiyin_rect_coverage(p, center, half_ext, radius);
  // replicated across RGBA -- the Gaussian runs on float4 planes
  write_imagef(out, (int2)(x, y), (float4)(a, a, a, 1.0f));
}

// box-average the source rect mapped to this output pixel (clamped -- the
// source may be any size; the rect edges are float-derived)
kernel void
yiyin_downsample(read_only image2d_t in,
                 write_only image2d_t out,
                 const int src_w,
                 const int src_h,
                 const int dst_w,
                 const int dst_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= dst_w || y >= dst_h) return;
  int x0 = (int)floor((float)x * src_w / dst_w);
  int y0 = (int)floor((float)y * src_h / dst_h);
  int x1 = (int)ceil((float)(x + 1) * src_w / dst_w);
  int y1 = (int)ceil((float)(y + 1) * src_h / dst_h);
  x1 = min(x1, src_w);
  y1 = min(y1, src_h);
  x0 = min(x0, x1 - 1);
  y0 = min(y0, y1 - 1);
  float4 acc = (float4)(0.0f);
  for(int sy = y0; sy < y1; sy++)
    for(int sx = x0; sx < x1; sx++)
      acc += read_imagef(in, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST, (int2)(sx, sy));
  float count = (float)((x1 - x0) * (y1 - y0));
  write_imagef(out, (int2)(x, y), acc / count);
}

kernel void
yiyin_composite(read_only image2d_t main_tex,
                read_only image2d_t blur_tex,
                read_only image2d_t shadow_tex,
                write_only image2d_t out,
                const int has_blur,
                const int has_overlay,
                const int has_shadow,
                const float fill_r, const float fill_g, const float fill_b,
                const float overlay_r, const float overlay_g,
                const float overlay_b, const float overlay_a,
                const float main_offset_x, const float main_offset_y,
                const float main_size_x, const float main_size_y,
                const int main_win_x,
                const int main_win_y,
                const float sdf_cx, const float sdf_cy,
                const float sdf_hx, const float sdf_hy,
                const float radius,
                const int output_w,
                const int output_h,
                const int canvas_w,
                const int canvas_h,
                const int plane_off_x,
                const int plane_off_y,
                const int blur_w,
                const int blur_h,
                const int sub_x, const int sub_y,
                const int sub_w, const int sub_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= output_w || y >= output_h) return;
  float2 p = (float2)(x, y) + 0.5f;
  int2 gid = (int2)(x, y);

  // 1. backdrop band: solid fill, or the blurred proxy sampled with
  //    normalized coords (aspect-preserving proxy; the canvas stretch is
  //    the yiyin genBlurImg resize shape)
  float4 o = (float4)(fill_r, fill_g, fill_b, 1.0f);
  if(has_blur)
  {
    // the backdrop is stretched across the CANVAS: this plane pixel's canvas
    // position is its position here plus the plane's canvas offset
    float2 canvas_p = p + (float2)(plane_off_x, plane_off_y);
    o = read_imagef(blur_tex, yiyin_sampler, canvas_p / (float2)(canvas_w, canvas_h));
  }
  if(has_overlay)
  {
    o.rgb = mix(o.rgb, (float3)(overlay_r, overlay_g, overlay_b), overlay_a);
  }

  // 2. shadow: the pre-blurred SDF alpha darkens beneath the picture
  if(has_shadow)
  {
    int2 sp = gid - (int2)(sub_x, sub_y);
    if(sp.x >= 0 && sp.y >= 0 && sp.x < sub_w && sp.y < sub_h)
    {
      float a = clamp(read_imagef(shadow_tex, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST, sp).r, 0.0f, 1.0f);
      o.rgb = mix(o.rgb, (float3)(0.0f), a);
    }
  }

  // 3. the picture, 1:1, rounded-rect clipped (radius 0 = the square path)
  float2 su = p - (float2)(main_offset_x, main_offset_y);
  if(su.x >= 0.0f && su.y >= 0.0f && su.x < main_size_x && su.y < main_size_y)
  {
    // main_tex holds the window that starts at (main_win_x, main_win_y)
    float4 m = read_imagef(main_tex, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST,
                           convert_int2(su) - (int2)(main_win_x, main_win_y));
    float cover = yiyin_rect_coverage(p, (float2)(sdf_cx, sdf_cy),
                                      (float2)(sdf_hx, sdf_hy), radius);
    o.rgb = mix(o.rgb, m.rgb, cover);
  }

  o.a = 1.0f; // the canvas is opaque
  write_imagef(out, gid, o);
}

// the film strip: picture 1:1 between two black bands, a centered row of
// light-box holes per band; the edge marking is composited separately
kernel void
yiyin_film_composite(read_only image2d_t main_tex,
                     write_only image2d_t out,
                     const int main_w, const int main_h,
                     const int output_w, const int output_h,
                     const int band,
                     const int hole_y0, const int hole_y1,
                     const int hole_w, const int pitch, const int first_hole,
                     const float black_r, const float black_g, const float black_b,
                     const float hole_r, const float hole_g, const float hole_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= output_w || y >= output_h) return;
  const int pic_y = y - band;
  if(pic_y >= 0 && pic_y < main_h)
  {
    float4 m = read_imagef(main_tex, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST,
                           (int2)(x, pic_y));
    m.a = 1.0f;
    write_imagef(out, (int2)(x, y), m);
    return;
  }
  // in a band; both bands use the same hole range
  const int band_ly = y < band ? y : y - band - main_h;
  float4 o;
  if(band_ly >= hole_y0 && band_ly < hole_y1
     && x >= first_hole && ((x - first_hole) % pitch) < hole_w)
    o = (float4)(hole_r, hole_g, hole_b, 1.0f);
  else
    o = (float4)(black_r, black_g, black_b, 1.0f);
  write_imagef(out, (int2)(x, y), o);
}

// mix the white-glyph coverage texture over the plane inside a bbox; the
// caller copies the bbox aside and hands it back as `in`
kernel void
yiyin_text_over(read_only image2d_t in,
                write_only image2d_t out,
                read_only image2d_t tex,
                const int bbox_x, const int bbox_y,
                const int dest_x, const int dest_y,
                const int tex_w, const int tex_h,
                const float text_r, const float text_g, const float text_b,
                const float text_opacity)
{
  const int gx = get_global_id(0);
  const int gy = get_global_id(1);
  const int ax = bbox_x + gx;
  const int ay = bbox_y + gy;
  float4 p = read_imagef(in, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST, (int2)(gx, gy));
  const int tx = ax - dest_x;
  const int ty = ay - dest_y;
  if(tx >= 0 && ty >= 0 && tx < tex_w && ty < tex_h)
  {
    const float a = clamp(read_imagef(tex, CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST,
                                      (int2)(tx, ty)).r, 0.0f, 1.0f)
                  * text_opacity;
    p.rgb = mix(p.rgb, (float3)(text_r, text_g, text_b), a);
  }
  write_imagef(out, (int2)(ax, ay), p);
}
