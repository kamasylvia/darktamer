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
#include "common/exif.h"
#include "common/image.h"
#include "common/mipmap_cache.h"
#include "imageio/imageio_ciraw.h"

#include <glib.h>

// shared GPU context: creating one per decode walks Metal device init,
// and mipmap loads run on several threads at once
static CIContext *ciraw_context = nil;
static pthread_mutex_t ciraw_context_lock = PTHREAD_MUTEX_INITIALIZER;

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
// neutralize every development stage dt owns itself, and also the whole
// tone/exposure group: dt's view transform (sigmoid, filmic, AgX or
// spektrafilm, auto-applied per workflow) is the only tone presentation,
// an Apple tone curve plus a dt view transform is two curves in series
// squeezing the gamut and lifting mid greens and cyans; Apple only
// decodes, dt presents: only extendedDynamicRangeAmount stays non-zero
// it is an output range declaration, not a curve, and keeps the HDR
// headroom the HDR flag downstream promises
static void _ciraw_neutralize(CIRAWFilter *filter)
{
  @autoreleasepool
  {
    // zeroed: dt's view transform owns the tone curve
    filter.boostAmount = 0.0f;
    // kept neutral: dt owns gamut mapping
    filter.gamutMappingEnabled = NO;
    // zeroed: headroom recovery is part of the Apple tone presentation
    filter.highlightRecoveryEnabled = NO;
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

    // force v9, never downgrade: a camera outside the v9 list is an error
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
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': camera not in the CIRAW v9 list",
               img->filename);
      return DT_IMAGEIO_UNSUPPORTED_CAMERA;
    }
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
    void *buf = dt_mipmap_cache_alloc(mbuf, img);
    if(!buf)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[ciraw_open] could not alloc full buffer for image `%s'",
               img->filename);
      return DT_IMAGEIO_CACHE_FULL;
    }

    CIContext *ctx = _ciraw_shared_context();
    if(ctx == nil)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ciraw_open] `%s': no Metal device for CIRAW",
               img->filename);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    if(cs == NULL)
    {
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
