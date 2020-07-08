/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm/gm.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkYUVAIndex.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrRecordingContext.h"
#include "src/core/SkAutoPixmapStorage.h"
#include "src/core/SkConvertPixels.h"
#include "src/core/SkScopeExit.h"
#include "tools/Resources.h"
#include "tools/ToolUtils.h"

namespace {
struct AsyncContext {
    bool fCalled = false;
    std::unique_ptr<const SkImage::AsyncReadResult> fResult;
};
}  // anonymous namespace

// Making this a lambda in the test functions caused:
//   "error: cannot compile this forwarded non-trivially copyable parameter yet"
// on x86/Win/Clang bot, referring to 'result'.
static void async_callback(void* c, std::unique_ptr<const SkImage::AsyncReadResult> result) {
    auto context = static_cast<AsyncContext*>(c);
    context->fResult = std::move(result);
    context->fCalled = true;
};

// Draws the image to a surface, does a asyncRescaleAndReadPixels of the image, and then sticks
// the result in a raster image.
template <typename Src>
static sk_sp<SkImage> do_read_and_scale(Src* src,
                                        GrContext* context,
                                        const SkIRect& srcRect,
                                        const SkImageInfo& ii,
                                        SkImage::RescaleGamma rescaleGamma,
                                        SkFilterQuality quality) {
    auto* asyncContext = new AsyncContext();
    src->asyncRescaleAndReadPixels(ii, srcRect, rescaleGamma, quality, async_callback,
                                   asyncContext);
    if (context) {
        context->submit();
    }
    while (!asyncContext->fCalled) {
        // Only GPU should actually be asynchronous.
        SkASSERT(context);
        context->checkAsyncWorkCompletion();
    }
    if (!asyncContext->fResult) {
        return nullptr;
    }
    SkPixmap pixmap(ii, asyncContext->fResult->data(0), asyncContext->fResult->rowBytes(0));
    auto releasePixels = [](const void*, void* c) { delete static_cast<AsyncContext*>(c); };
    return SkImage::MakeFromRaster(pixmap, releasePixels, asyncContext);
}

template <typename Src>
static sk_sp<SkImage> do_read_and_scale_yuv(Src* src,
                                            GrContext* context,
                                            SkYUVColorSpace yuvCS,
                                            const SkIRect& srcRect,
                                            SkISize size,
                                            SkImage::RescaleGamma rescaleGamma,
                                            SkFilterQuality quality,
                                            SkScopeExit* cleanup) {
    SkASSERT(!(size.width() & 0b1) && !(size.height() & 0b1));

    SkISize uvSize = {size.width()/2, size.height()/2};
    SkImageInfo yII  = SkImageInfo::Make(size,   kGray_8_SkColorType, kPremul_SkAlphaType);
    SkImageInfo uvII = SkImageInfo::Make(uvSize, kGray_8_SkColorType, kPremul_SkAlphaType);

    AsyncContext asyncContext;
    src->asyncRescaleAndReadPixelsYUV420(yuvCS, SkColorSpace::MakeSRGB(), srcRect, size,
                                         rescaleGamma, quality, async_callback, &asyncContext);
    if (context) {
        context->submit();
    }
    while (!asyncContext.fCalled) {
        // Only GPU should actually be asynchronous.
        SkASSERT(context);
        context->checkAsyncWorkCompletion();
    }
    if (!asyncContext.fResult) {
        return nullptr;
    }
    GrBackendTexture backendTextures[3];

    SkPixmap yPM(yII, asyncContext.fResult->data(0), asyncContext.fResult->rowBytes(0));
    SkPixmap uPM(uvII, asyncContext.fResult->data(1), asyncContext.fResult->rowBytes(1));
    SkPixmap vPM(uvII, asyncContext.fResult->data(2), asyncContext.fResult->rowBytes(2));

    backendTextures[0] = context->createBackendTexture(yPM, GrRenderable::kNo, GrProtected::kNo);
    backendTextures[1] = context->createBackendTexture(uPM, GrRenderable::kNo, GrProtected::kNo);
    backendTextures[2] = context->createBackendTexture(vPM, GrRenderable::kNo, GrProtected::kNo);

    SkYUVAIndex indices[4] = {
        { 0, SkColorChannel::kR},
        { 1, SkColorChannel::kR},
        { 2, SkColorChannel::kR},
        {-1, SkColorChannel::kR}
    };

    *cleanup = {[context, backendTextures] {
        context->flush();
        context->submit(true);
        context->deleteBackendTexture(backendTextures[0]);
        context->deleteBackendTexture(backendTextures[1]);
        context->deleteBackendTexture(backendTextures[2]);
    }};

    return SkImage::MakeFromYUVATextures(context, yuvCS, backendTextures, indices, size,
                                         kTopLeft_GrSurfaceOrigin, SkColorSpace::MakeSRGB());
}

// Draws a grid of rescales. The columns are none, low, and high filter quality. The rows are
// rescale in src gamma and rescale in linear gamma.
template <typename Src>
static skiagm::DrawResult do_rescale_grid(SkCanvas* canvas,
                                          Src* src,
                                          GrContext* context,
                                          const SkIRect& srcRect,
                                          SkISize newSize,
                                          bool doYUV420,
                                          SkString* errorMsg,
                                          int pad = 0) {
    if (doYUV420 && !GrAsDirectContext(canvas->recordingContext())) {
        errorMsg->printf("YUV420 only supported on direct GPU for now.");
        return skiagm::DrawResult::kSkip;
    }
    if (canvas->imageInfo().colorType() == kUnknown_SkColorType) {
        *errorMsg = "Not supported on recording/vector backends.";
        return skiagm::DrawResult::kSkip;
    }
    const auto ii = canvas->imageInfo().makeDimensions(newSize);

    SkYUVColorSpace yuvColorSpace = kRec601_SkYUVColorSpace;
    canvas->save();
    for (auto gamma : {SkImage::RescaleGamma::kSrc, SkImage::RescaleGamma::kLinear}) {
        canvas->save();
        for (auto quality : {kNone_SkFilterQuality, kLow_SkFilterQuality, kHigh_SkFilterQuality}) {
            SkScopeExit cleanup;
            sk_sp<SkImage> result;
            if (doYUV420) {
                result = do_read_and_scale_yuv(src, context, yuvColorSpace, srcRect, newSize, gamma,
                                               quality, &cleanup);
                if (!result) {
                    errorMsg->printf("YUV420 async call failed. Allowed for now.");
                    return skiagm::DrawResult::kSkip;
                }
                int nextCS = static_cast<int>(yuvColorSpace + 1) % (kLastEnum_SkYUVColorSpace + 1);
                yuvColorSpace = static_cast<SkYUVColorSpace>(nextCS);
            } else {
                result = do_read_and_scale(src, context, srcRect, ii, gamma, quality);
                if (!result) {
                    errorMsg->printf("async read call failed.");
                    return skiagm::DrawResult::kFail;
                }
            }
            canvas->drawImage(result, 0, 0);
            canvas->translate(newSize.width() + pad, 0);
        }
        canvas->restore();
        canvas->translate(0, newSize.height() + pad);
    }
    canvas->restore();
    return skiagm::DrawResult::kOk;
}

static skiagm::DrawResult do_rescale_image_grid(SkCanvas* canvas,
                                                const char* imageFile,
                                                const SkIRect& srcRect,
                                                SkISize newSize,
                                                bool doSurface,
                                                bool doYUV420,
                                                SkString* errorMsg) {
    auto image = GetResourceAsImage(imageFile);
    if (!image) {
        errorMsg->printf("Could not load image file %s.", imageFile);
        return skiagm::DrawResult::kFail;
    }
    if (canvas->imageInfo().colorType() == kUnknown_SkColorType) {
        *errorMsg = "Not supported on recording/vector backends.";
        return skiagm::DrawResult::kSkip;
    }
    if (doSurface) {
        // Turn the image into a surface in order to call the read and rescale API
        auto surfInfo = image->imageInfo().makeDimensions(image->dimensions());
        auto surface = canvas->makeSurface(surfInfo);
        if (!surface && surfInfo.colorType() == kBGRA_8888_SkColorType) {
            surfInfo = surfInfo.makeColorType(kRGBA_8888_SkColorType);
            surface = canvas->makeSurface(surfInfo);
        }
        if (!surface) {
            *errorMsg = "Could not create surface for image.";
            // When testing abandoned GrContext we expect surface creation to fail.
            if (canvas->recordingContext() && canvas->recordingContext()->abandoned()) {
                return skiagm::DrawResult::kSkip;
            }
            return skiagm::DrawResult::kFail;
        }
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        surface->getCanvas()->drawImage(image, 0, 0, &paint);
        return do_rescale_grid(canvas, surface.get(), canvas->getGrContext(), srcRect, newSize,
                               doYUV420, errorMsg);
    } else if (auto ctx = canvas->getGrContext()) {
        image = image->makeTextureImage(ctx);
        if (!image) {
            *errorMsg = "Could not create image.";
            // When testing abandoned GrContext we expect surface creation to fail.
            if (canvas->recordingContext() && canvas->recordingContext()->abandoned()) {
                return skiagm::DrawResult::kSkip;
            }
            return skiagm::DrawResult::kFail;
        }
    }
    return do_rescale_grid(canvas, image.get(), canvas->getGrContext(), srcRect, newSize, doYUV420,
                           errorMsg);
}

#define DEF_RESCALE_AND_READ_SURF_GM(IMAGE_FILE, TAG, SRC_RECT, W, H)                      \
    DEF_SIMPLE_GM_CAN_FAIL(async_rescale_and_read_##TAG, canvas, errorMsg, 3 * W, 2 * H) { \
        ToolUtils::draw_checkerboard(canvas, SK_ColorDKGRAY, SK_ColorLTGRAY, 25);          \
        return do_rescale_image_grid(canvas, #IMAGE_FILE, SRC_RECT, {W, H}, true, false,   \
                                     errorMsg);                                            \
    }

#define DEF_RESCALE_AND_READ_YUV_SURF_GM(IMAGE_FILE, TAG, SRC_RECT, W, H)                          \
    DEF_SIMPLE_GM_CAN_FAIL(async_rescale_and_read_yuv420_##TAG, canvas, errorMsg, 3 * W, 2 * H) {  \
        ToolUtils::draw_checkerboard(canvas, SK_ColorDKGRAY, SK_ColorLTGRAY, 25);                  \
        return do_rescale_image_grid(canvas, #IMAGE_FILE, SRC_RECT, {W, H}, true, true, errorMsg); \
    }

#define DEF_RESCALE_AND_READ_IMG_GM(IMAGE_FILE, TAG, SRC_RECT, W, H)                       \
    DEF_SIMPLE_GM_CAN_FAIL(async_rescale_and_read_##TAG, canvas, errorMsg, 3 * W, 2 * H) { \
        ToolUtils::draw_checkerboard(canvas, SK_ColorDKGRAY, SK_ColorLTGRAY, 25);          \
        return do_rescale_image_grid(canvas, #IMAGE_FILE, SRC_RECT, {W, H}, false, false,  \
                                     errorMsg);                                            \
    }

#define DEF_RESCALE_AND_READ_YUV_IMG_GM(IMAGE_FILE, TAG, SRC_RECT, W, H)                           \
    DEF_SIMPLE_GM_CAN_FAIL(async_rescale_and_read_yuv420_##TAG, canvas, errorMsg, 3 * W, 2 * H) {  \
        ToolUtils::draw_checkerboard(canvas, SK_ColorDKGRAY, SK_ColorLTGRAY, 25);                  \
        return do_rescale_image_grid(canvas, #IMAGE_FILE, SRC_RECT, {W, H}, true, true, errorMsg); \
    }

DEF_RESCALE_AND_READ_YUV_SURF_GM(
        images/yellow_rose.webp, rose, SkIRect::MakeXYWH(50, 5, 200, 150), 410, 376)

DEF_RESCALE_AND_READ_YUV_IMG_GM(
        images/yellow_rose.webp, rose_down, SkIRect::MakeXYWH(50, 5, 200, 150), 106, 60)

DEF_RESCALE_AND_READ_SURF_GM(
        images/yellow_rose.webp, rose, SkIRect::MakeXYWH(100, 20, 100, 100), 410, 410)

DEF_RESCALE_AND_READ_SURF_GM(images/dog.jpg, dog_down, SkIRect::MakeXYWH(0, 10, 180, 150), 45, 45)
DEF_RESCALE_AND_READ_IMG_GM(images/dog.jpg, dog_up, SkIRect::MakeWH(180, 180), 800, 400)

DEF_RESCALE_AND_READ_IMG_GM(
        images/text.png, text_down, SkIRect::MakeWH(637, 105), (int)(0.7 * 637), (int)(0.7 * 105))
DEF_RESCALE_AND_READ_SURF_GM(
        images/text.png, text_up, SkIRect::MakeWH(637, 105), (int)(1.2 * 637), (int)(1.2 * 105))
DEF_RESCALE_AND_READ_IMG_GM(images/text.png,
                            text_up_large,
                            SkIRect::MakeXYWH(300, 0, 300, 105),
                            (int)(2.4 * 300),
                            (int)(2.4 * 105))

// Exercises non-scaling YUV420. Reads from the original canvas's surface in order to
// exercise case where source surface is not a texture (in glbert config).
DEF_SIMPLE_GM_CAN_FAIL(async_yuv_no_scale, canvas, errorMsg, 400, 300) {
    auto surface = canvas->getSurface();
    if (!surface) {
        *errorMsg = "Not supported on recording/vector backends.";
        return skiagm::DrawResult::kSkip;
    }

    auto image = GetResourceAsImage("images/yellow_rose.webp");
    if (!image) {
        return skiagm::DrawResult::kFail;
    }
    SkPaint paint;
    canvas->drawImage(image.get(), 0, 0);

    auto direct = surface->recordingContext() ? surface->recordingContext()->asDirectContext()
                                              : nullptr;

    SkScopeExit scopeExit;
    auto yuvImage = do_read_and_scale_yuv(
            surface, direct, kRec601_SkYUVColorSpace, SkIRect::MakeWH(400, 300),
            {400, 300}, SkImage::RescaleGamma::kSrc, kNone_SkFilterQuality, &scopeExit);

    canvas->clear(SK_ColorWHITE);
    canvas->drawImage(yuvImage.get(), 0, 0);

    return skiagm::DrawResult::kOk;
}

DEF_SIMPLE_GM_CAN_FAIL(async_rescale_and_read_no_bleed, canvas, errorMsg, 60, 60) {
    if (canvas->imageInfo().colorType() == kUnknown_SkColorType) {
        *errorMsg = "Not supported on recording/vector backends.";
        return skiagm::DrawResult::kSkip;
    }

    static constexpr int kBorder = 5;
    static constexpr int kInner = 5;
    const auto srcRect = SkIRect::MakeXYWH(kBorder, kBorder, kInner, kInner);
    auto surfaceII =
            SkImageInfo::Make(kInner + 2 * kBorder, kInner + 2 * kBorder, kRGBA_8888_SkColorType,
                              kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = canvas->makeSurface(surfaceII);
    if (!surface) {
        *errorMsg = "Could not create surface for image.";
        // When testing abandoned GrContext we expect surface creation to fail.
        if (canvas->getGrContext() && canvas->getGrContext()->abandoned()) {
            return skiagm::DrawResult::kSkip;
        }
        return skiagm::DrawResult::kFail;
    }
    surface->getCanvas()->clear(SK_ColorRED);
    surface->getCanvas()->save();
    surface->getCanvas()->clipRect(SkRect::Make(srcRect), SkClipOp::kIntersect, false);
    surface->getCanvas()->clear(SK_ColorBLUE);
    surface->getCanvas()->restore();
    static constexpr int kPad = 2;
    canvas->translate(kPad, kPad);
    skiagm::DrawResult result;
    SkISize downSize = {static_cast<int>(kInner/2),  static_cast<int>(kInner / 2)};
    GrContext* context = canvas->getGrContext();
    result = do_rescale_grid(canvas, surface.get(), context, srcRect, downSize, false, errorMsg,
                             kPad);

    if (result != skiagm::DrawResult::kOk) {
        return result;
    }
    canvas->translate(0, 4 * downSize.height());
    SkISize upSize = {static_cast<int>(kInner * 3.5), static_cast<int>(kInner * 4.6)};
    result =
            do_rescale_grid(canvas, surface.get(), context, srcRect, upSize, false, errorMsg, kPad);
    if (result != skiagm::DrawResult::kOk) {
        return result;
    }
    return skiagm::DrawResult::kOk;
}
