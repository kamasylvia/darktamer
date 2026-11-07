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

/* workaround to fix issue #12720 */
#define _DARWIN_C_SOURCE

#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>

#include "common/darktable.h"
#include "common/database.h"
#include "common/exif.h"
#include "control/conf.h"
#include "common/image.h"
#include "common/mipmap_cache.h"
#include "imageio/imageio_ciraw.h"

#include <sqlite3.h>

#include <glib.h>

// shared GPU context: creating one per decode walks Metal device init,
// and mipmap loads run on several threads at once
static CIContext *ciraw_context = nil;
static pthread_mutex_t ciraw_context_lock = PTHREAD_MUTEX_INITIALIZER;

// decode concurrency cap: each in-flight decode holds a full-frame float
// destination (~963MB for a 60MP raw) plus CoreImage working memory. The
// mipmap job queue runs on up to a dozen threads and would otherwise spike
// several GB during thumbnail regeneration on a 16G machine; two concurrent
// decodes keep the GPU busy without the swap storm
static dispatch_semaphore_t ciraw_decode_sem;

static void _ciraw_decode_gate_enter(void)
{
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    // clamped to 1..8 by the cache_ciraw_decodes config range
    int conf = dt_conf_get_int("cache_ciraw_decodes");
    if(conf < 1) conf = 1;
    if(conf > 8) conf = 8;
    ciraw_decode_sem = dispatch_semaphore_create(conf);
  });
  dispatch_semaphore_wait(ciraw_decode_sem, DISPATCH_TIME_FOREVER);
}

static void _ciraw_decode_gate_leave(void)
{
  dispatch_semaphore_signal(ciraw_decode_sem);
}

static CIContext *_ciraw_shared_context(void)
{
  @autoreleasepool
  {
    pthread_mutex_lock(&ciraw_context_lock);
    if(ciraw_context == nil)
    {
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      if(dev != nil)
        ciraw_context = [[CIContext contextWithMTLDevice:dev] retain];
    }
    CIContext *ctx = ciraw_context;
    pthread_mutex_unlock(&ciraw_context_lock);
    return ctx;
  }
}

static CIRAWDecoderVersion _ciraw_pick_version(CIRAWFilter *filter, NSString *path)
{
  // newest decoder the camera offers: v9/v9DNG when listed, otherwise
  // the highest legacy version. The v9 class-level list contains entries
  // whose names never match a real body's EXIF model string ("Sony Alpha
  // ILCE-7R V" vs EXIF "ILCE-7RM5"), so per-file supportedDecoderVersions
  // omits v9 for them and hard-requiring v9 would keep those bodies
  // unusable; falling back keeps the decode inside the CIRAW pipeline.
  // nil means the camera offers no CIRAW decoder version at all
  NSArray<CIRAWDecoderVersion> *versions = filter.supportedDecoderVersions;
  CIRAWDecoderVersion want = nil;
  if(@available(macOS 27.0, *))
  {
    if([versions containsObject:CIRAWDecoderVersion9])
      want = CIRAWDecoderVersion9;
    else if([versions containsObject:CIRAWDecoderVersion9DNG])
      want = CIRAWDecoderVersion9DNG;
  }
  if(want == nil)
  {
    // legacy picks come in two flavors, "8" and "8.dng"; a DNG must get
    // the .dng flavor (measured 30% mean deviation when it does not) and
    // a non-DNG the plain one, so filter by flavor first and take the
    // highest version within it
    NSString *ext = [path lowercaseString];
    const BOOL wants_dng_flavor = [ext hasSuffix:@".dng"];
    int best = -1;
    for(int pass = 0; pass < 2 && want == nil; pass++)
    {
      for(NSString *v in versions)
      {
        if(pass == 0 && [v hasSuffix:@".dng"] != (BOOL)wants_dng_flavor)
          continue;
        const int n = [v intValue];
        if(n > best)
        {
          best = n;
          want = v;
        }
      }
    }
  }
  return want;
}
// neutralize every development stage dt owns itself, and also the whole
// tone/exposure group (boost/shadowBias): dt's view transform (sigmoid,
// filmic, AgX or spektrafilm, auto-applied per workflow) is the only tone
// presentation, an Apple tone curve plus a dt view transform is two curves
// in series squeezing the gamut and lifting mid greens and cyans; Apple
// only decodes, dt presents: only extendedDynamicRangeAmount stays non-zero
// (an output range declaration, not a curve, keeps the HDR headroom the HDR
// flag downstream promises) and highlight recovery stays on (superwhite hue
// reconstruction, not a curve: see the knob comment below)
static void _ciraw_neutralize(CIRAWFilter *filter)
{
  @autoreleasepool
  {
    // zeroed: dt's view transform owns the tone curve
    filter.boostAmount = 0.0f;
    // kept neutral: dt owns gamut mapping
    filter.gamutMappingEnabled = NO;
    // kept: NOT a tone curve knob, it reconstructs hue in clipped superwhite
    // data before EDR range mapping; without it over-range channels saturate
    // unevenly and the dt highlights module never sees developed-RGB whites,
    // so highlights render magenta (batch-9 probe hr.mm: H0 R/B 1.268,
    // magenta-frac 2.08% -> H1 R/B 1.197, magenta-frac 0.00%)
    // the knob only exists from macOS 26 on; without it the Apple default
    // applies, which is also on
    if(@available(macOS 26.0, *))
    {
      filter.highlightRecoveryEnabled = YES;
    }
    // kept neutral: dt's lens module handles correction
    filter.lensCorrectionEnabled = NO;
    // kept neutral: dt denoises
    filter.luminanceNoiseReductionAmount = 0.0f;
    // kept neutral: dt denoises
    filter.colorNoiseReductionAmount = 0.0f;
    // kept neutral: dt sharpens
    filter.sharpnessAmount = 0.0f;
    // kept neutral: dt sets contrast
    filter.contrastAmount = 0.0f;
    // kept neutral: dt owns detail
    filter.detailAmount = 0.0f;
    // kept neutral: dt owns detail
    filter.moireReductionAmount = 0.0f;
    // kept neutral: dt owns detail
    filter.despeckleAmount = 0.0f;
    // 0.0 is already the v9 default: nothing to restore
    filter.localToneMapAmount = 0.0f;
    // kept neutral: 0 EV, the filter default
    filter.exposure = 0.0f;
    // zeroed: shadow lift is part of the Apple tone presentation
    filter.shadowBias = 0.0f;
    // kept: not the v9 default (0.0), but full-range output is what the
    // HDR flag downstream promises
    filter.extendedDynamicRangeAmount = 2.0f;
  }
}

dt_imageio_retval_t dt_imageio_open_ciraw(dt_image_t *img, const char *filename,
                                          dt_mipmap_buffer_t *mbuf)
{
  @autoreleasepool
  {
    if(!img)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] failed to get dt_image_t for '%s' at %p",
               filename, (const void *)mbuf);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    if(!img->exif_inited)
      (void)dt_exif_read(img, filename);

    if(@available(macOS 27.0, *))
    {
    }
    else
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': CIRAW v9 needs macOS 27 or newer",
               img->filename);
      return DT_IMAGEIO_UNSUPPORTED_CAMERA;
    }

    NSString *path = [NSString stringWithUTF8String:filename];
    NSURL *url = [NSURL fileURLWithPath:path];
    CIRAWFilter *filter = [CIRAWFilter filterWithImageURL:url];
    if(filter == nil)
      return DT_IMAGEIO_UNSUPPORTED_FORMAT;

    // newest decoder the camera offers: v9/v9DNG when listed, otherwise
    // the highest legacy version. The v9 class-level list contains
    // entries whose names never match a real body's EXIF model string
    // ("Sony Alpha ILCE-7R V" vs EXIF "ILCE-7RM5"), so per-file
    // supportedDecoderVersions omits v9 for them and hard-requiring v9
    // would keep those bodies unusable. Falling back keeps the decode
    // inside the CIRAW pipeline (same neutralize + dt view transform);
    // same-knobs v8/v9 dual decodes match within ~1.5% on channel means
    // (dev-doc/CIRAW_Decoder.md, "Coverage and fallback")
    CIRAWDecoderVersion want = _ciraw_pick_version(filter, path);
    if(want == nil)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': camera offers no CIRAW decoder version",
               img->filename);
      return DT_IMAGEIO_UNSUPPORTED_CAMERA;
    }
    if([want intValue] < 9)
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': camera not on the v9 list, decoding with v%d",
               img->filename, [want intValue]);
    filter.decoderVersion = want;

    // camera resources for newly released bodies may need an OTA download
    __block BOOL done = NO;
    __block NSError *dl_err = nil;
    if(@available(macOS 27.0, *))
    {
      [filter downloadResourcesWithTimeout:10.0
                         completionHandler:^(NSError *err) { dl_err = err; done = YES; }];
    }
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:11.0];
    while(!done && [limit timeIntervalSinceNow] > 0)
      [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                               beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    if(!done || dl_err != nil)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[ciraw_open] `%s': camera resources need an online download (%s)",
               img->filename, dl_err != nil ? [[dl_err localizedDescription] UTF8String] : "timed out");
      return DT_IMAGEIO_UNSUPPORTED_CAMERA;
    }

    _ciraw_neutralize(filter);

    // orientation Up before reading the extent: the default EXIF rotation
    // would come out sideways here and stack with dt's own flip
    filter.orientation = kCGImagePropertyOrientationUp;
    CIImage *out = filter.outputImage;
    if(out == nil)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': CIRAW v9 produced no image",
               img->filename);
      return DT_IMAGEIO_UNSUPPORTED_FORMAT;
    }
    CGRect extent = out.extent;
    const int width = (int)CGRectGetWidth(extent);
    const int height = (int)CGRectGetHeight(extent);
    if(width <= 0 || height <= 0)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': CIRAW v9 gave an empty extent",
               img->filename);
      return DT_IMAGEIO_UNSUPPORTED_FORMAT;
    }

    img->width = width;
    img->height = height;
    img->buf_dsc.channels = 4;
    img->buf_dsc.datatype = TYPE_FLOAT;
    img->buf_dsc.filters = 0u;
    img->buf_dsc.cst = IOP_CS_RGB;

    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags |= DT_IMAGE_CIRAW;
    img->flags |= DT_IMAGE_HDR;

    // developed output is already normalized: white maps to 1, so no
    // leftover scaling may touch it downstream
    img->raw_white_point = 1;
    for(int k = 0; k < 4; k++) img->buf_dsc.processed_maximum[k] = 1.0f;

    img->loader = LOADER_CIRAW;

    // probe call: metadata only, no pixels yet
    if(!mbuf)
      return DT_IMAGEIO_OK;

    // render straight into the mipmap buffer: a 60MP RGBA float frame is
    // ~0.9GB, a temp copy would double the peak
    _ciraw_decode_gate_enter();
    void *buf = dt_mipmap_cache_alloc(mbuf, img);
    if(!buf)
    {
      _ciraw_decode_gate_leave();
      dt_print(DT_DEBUG_ALWAYS,
               "[ciraw_open] could not alloc full buffer for image `%s'",
               img->filename);
      return DT_IMAGEIO_CACHE_FULL;
    }

    CIContext *ctx = _ciraw_shared_context();
    if(ctx == nil)
    {
      _ciraw_decode_gate_leave();
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': no Metal device for CIRAW",
               img->filename);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    if(cs == NULL)
    {
      _ciraw_decode_gate_leave();
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': no extended linear sRGB space",
               img->filename);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    dt_imageio_retval_t ret = DT_IMAGEIO_OK;
    // probe-verified: concurrent renders on the shared context stay
    // bit-identical, so only the context fetch above takes the lock
    const size_t rowbytes = (size_t)width * 4 * sizeof(float);
    CIRenderDestination *dest =
        [[CIRenderDestination alloc] initWithBitmapData:buf
                                                  width:(NSUInteger)width
                                                 height:(NSUInteger)height
                                            bytesPerRow:rowbytes
                                                 format:kCIFormatRGBAf];
    dest.colorSpace = cs;
    // dt buffers run top-down, Core Image destinations bottom-up
    dest.flipped = YES;
    // alpha channel carries nothing, keep it out of the blend
    dest.alphaMode = CIRenderDestinationAlphaNone;
    NSError *err = nil;
    CIRenderTask *task = [ctx startTaskToRender:out
                                      fromRect:extent
                                   toDestination:dest
                                         atPoint:CGPointZero
                                           error:&err];
    if(task == nil)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': render failed to start (%s)",
               img->filename, err != nil ? [[err localizedDescription] UTF8String] : "no error");
      ret = DT_IMAGEIO_LOAD_FAILED;
    }
    else
    {
      [task waitUntilCompletedAndReturnError:&err];
      if(err != nil)
      {
        dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': render failed (%s)",
                 img->filename, [[err localizedDescription] UTF8String]);
        ret = DT_IMAGEIO_LOAD_FAILED;
      }
    }
    CGColorSpaceRelease(cs);
    _ciraw_decode_gate_leave();
    if(ret != DT_IMAGEIO_OK)
      return ret;

    // alpha is undefined after the render, pin it to 0 like the sraw path
    DT_OMP_FOR()
    for(int j = 0; j < height; j++)
    {
      float *row = ((float *)buf) + (size_t)4 * width * j;
      for(int i = 0; i < width; i++)
        row[i * 4 + 3] = 0.0f;
    }

    return DT_IMAGEIO_OK;
  }
}

// render one small frame with the given decoder version; records the
// version in seen[] when new. returns TRUE when a version got warmed
static gboolean _ciraw_warm_version(CIContext *ctx, int *seen, int *nseen,
                                    CIRAWFilter *f, CIRAWDecoderVersion want,
                                    const char *path)
{
  @autoreleasepool
  {
    const int ver = [want intValue];
    for(int i = 0; i < *nseen; i++)
      if(seen[i] == ver) return FALSE;
    if(*nseen >= 8) return FALSE;
    f.decoderVersion = want;
    CIImage *out = f.outputImage;
    if(out == nil) return FALSE;
    CGRect ext = out.extent;
    const CGFloat scale = 2048.0 / MAX(ext.size.width, ext.size.height);
    CIImage *scaled = [out imageByApplyingTransform:
      CGAffineTransformMakeScale(scale, scale)];
    ext = scaled.extent;
    const size_t w = (size_t)CGRectGetWidth(ext), h = (size_t)CGRectGetHeight(ext);
    NSMutableData *buf = [NSMutableData dataWithLength:w * h * 4 * sizeof(float)];
    if(buf == NULL) return FALSE;
    CIRenderDestination *dest = [[CIRenderDestination alloc]
      initWithBitmapData:buf.mutableBytes width:w height:h
      bytesPerRow:w * 4 * sizeof(float) format:kCIFormatRGBAf];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    dest.colorSpace = cs;
    CGColorSpaceRelease(cs);
    dest.flipped = YES;
    dest.alphaMode = CIRenderDestinationAlphaNone;
    NSError *err = nil;
    CIRenderTask *task = [ctx startTaskToRender:scaled fromRect:ext
                                  toDestination:dest atPoint:CGPointZero
                                          error:&err];
    if(task == nil) return FALSE;
    [task waitUntilCompletedAndReturnError:&err];
    seen[(*nseen)++] = ver;
    dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] warmup decode `%s' with v%d done (%d/2)",
             path, ver, *nseen);
    return TRUE;
  }
}

// the first decode after launch pays a one-time CoreImage Metal kernel
// build, 30-50s per decoder pipeline on this machine, which would land on
// the first image the user opens. Warm the pipelines in the background
// with a small decode each, stopping once two distinct decoder versions
// have rendered; later decodes cost ~1s
static void *_ciraw_warmup_thread(void *arg)
{
  @autoreleasepool
  {
    if(@available(macOS 27.0, *))
    {
    }
    else
      return NULL;
    sqlite3 *db = dt_database_get(darktable.db);
    if(db == NULL) return NULL;
    sqlite3_stmt *stmt = NULL;
    static const char *q =
      "SELECT folder || '/' || filename FROM main.images"
      " JOIN main.film_rolls ON film_id = film_rolls.id"
      " WHERE lower(substr(filename, -4)) IN"
      "       ('.arw','.dng','.cr2','.cr3','.nef','.nrw','.raf'"
      "       ,'.orf','.rw2','.srw','.pef','.raw','.rwl','.iiq')"
      " ORDER BY images.id LIMIT 64";
    if(sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    CIContext *ctx = _ciraw_shared_context();
    int seen[8] = { 0 };
    int nseen = 0;
    while(sqlite3_step(stmt) == SQLITE_ROW && nseen < 2)
    {
      const char *path = (const char *)sqlite3_column_text(stmt, 0);
      if(path == NULL) continue;
      NSString *p = [NSString stringWithUTF8String:path];
      CIRAWFilter *f = [CIRAWFilter filterWithImageURL:[NSURL fileURLWithPath:p]];
      if(f == nil) continue;
      CIRAWDecoderVersion want = _ciraw_pick_version(f, p);
      if(want == nil) continue;
      _ciraw_neutralize(f);
      _ciraw_warm_version(ctx, seen, &nseen, f, want, path);
      // a v9 warmup does not cover the legacy kernels the downgrade path
      // uses, so give files that offer both a second decode with the
      // highest legacy version
      if([want intValue] >= 9)
      {
        CIRAWDecoderVersion legacy = nil;
        int best = -1;
        for(NSString *v in f.supportedDecoderVersions)
        {
          const int n = [v intValue];
          if(n > 0 && n < 9 && n > best)
          {
            best = n;
            legacy = v;
          }
        }
        if(legacy != nil)
          _ciraw_warm_version(ctx, seen, &nseen, f, legacy, path);
      }
    }
    sqlite3_finalize(stmt);
    if(nseen > 0)
      dt_print(DT_DEBUG_ALWAYS,
               "[ciraw_open] warmup finished, %d decoder pipeline(s) compiled", nseen);
  }
  return NULL;
}

void dt_ciraw_warmup_start(void)
{
  g_thread_new("ciraw-warmup", _ciraw_warmup_thread, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
