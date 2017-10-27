/*
 * Copyright (c) 2017 Intel Corporation, all rights reserved.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * libxcam wrapper functions
 */

#include <strings.h>

#include <xcam/capi/xcam_handle.h>

#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"

#define XCAM_MAX_PARAMS_COUNT  20
#define XCAM_MAX_PARAM_LENGTH  255

/**
 * video filter features supported by libXCam.
 */
typedef enum XCAMFilterID {
    XCAM_NONE,
    XCAM_3DNR,
    XCAM_WAVELETNR,
    XCAM_FISHEYE,
    XCAM_DEFOG,
    XCAM_DVS,
    XCAM_STITCH,
} XCAMFilterID;

typedef struct XCAMFilterEntry {
    XCAMFilterID id;
    const char* name;
} XCAMFilterEntry;

static const XCAMFilterEntry xcam_filter_entries[] = {
    {XCAM_NONE, "None" },
    {XCAM_3DNR, "3DNR" },
    {XCAM_WAVELETNR, "WaveletNR" },
    {XCAM_FISHEYE, "Fisheye" },
    {XCAM_DEFOG, "Defog" },
    {XCAM_DVS, "DVS" },
    {XCAM_STITCH, "Stitch" },
};

typedef struct XCAMContext {
    const AVClass *class;
    char *name;
    XCamHandle* handle;
    int  w;
    int  h;
    char *params;
    int (*init)(AVFilterContext *ctx, const char *args);
    void (*uninit)(AVFilterContext *ctx);
    void (*execute)(AVFilterContext *ctx, XCamVideoBuffer *inimg, XCamVideoBuffer *outimg);
    void *priv;
} XCAMContext;

/**
 * libXCam buffer struct.
 * XCamVideoBuffer* buffer struct used internally in libXCam
 * AVFrame* FFMpeg AVFrame attached to libXCam buffer struct
 */
typedef struct XCamVideoFilterBuf {
    XCamVideoBuffer buf;
    AVFrame* av_buf;
} XCamVideoFilterBuf;

/**
 * Implementation of buffer ref function defined in libXCam video buffer function table.
 *
 * @param XCamVideoBuffer* libXCam buffer pointer
 * @return void
 */
static void xcam_video_filter_buf_ref (XCamVideoBuffer* buf) {
    return;
}

/**
 * Implementation of buffer unref function defined in libXCam video buffer function table.
 *
 * @param XCamVideoBuffer* libXCam buffer pointer
 * @return void
 */
static void xcam_video_filter_buf_unref (XCamVideoBuffer* buf) {
    return;
}

/**
 * Implementation of buffer map function defined in libXCam video buffer function table.
 *
 * @param XCamVideoBuffer* libXCam buffer pointer
 * @return uint8_t* FFMpeg AVFrame buffer pointer
 */
static uint8_t* xcam_video_filter_buf_map (XCamVideoBuffer* buf) {
    XCamVideoFilterBuf* avfilter_buf = (XCamVideoFilterBuf*)(buf);
    return avfilter_buf->av_buf->data[0];
}

/**
 * Implementation of buffer unmap function defined in libXCam video buffer function table.
 *
 * @param XCamVideoBuffer* libXCam buffer pointer
 * @return void
 */
static void xcam_video_filter_buf_unmap (XCamVideoBuffer* buf) {
    return;
}

/**
 * Implementation of get_fd function defined in libXCam video buffer function table.
 *
 * @param XCamVideoBuffer* libXCam buffer pointer
 * @return 1 for success
 */
static int xcam_video_filter_buf_get_fd (XCamVideoBuffer* buf) {
    return 1;
}

/**
 * Attach FFMpeg AVFrame buffer to libXCam internal buffer
 *
 * @param XCamVideoFilterBuf* buffer used in libXCam internally
 * @param AVFrame* FFMpeg video filter buffer
 * @param AVPixelFormat only support AV_PIX_FMT_NV12
 * @return void
 */
static void fill_xcam_buffer_from_frame(XCamVideoFilterBuf *img, AVFrame *frame, enum AVPixelFormat pixfmt)
{
    if (pixfmt != AV_PIX_FMT_NV12) {
        return;
    }

    img->buf.ref = xcam_video_filter_buf_ref;
    img->buf.unref = xcam_video_filter_buf_unref;
    img->buf.map = xcam_video_filter_buf_map;
    img->buf.unmap = xcam_video_filter_buf_unmap;
    img->buf.get_fd = xcam_video_filter_buf_get_fd;

    img->av_buf = frame;
}

/**
 * Copy libXCam internal buffer to FFMpeg AVFrame buffer
 *
 * @param AVFrame* FFMpeg video filter buffer
 * @param XCamVideoFilterBuf* buffer used in libXCam internally
 * @param AVPixelFormat only support AV_PIX_FMT_NV12
 * @return void
 */
static void fill_frame_from_xcam_buffer(AVFrame *frame, XCamVideoBuffer *img, enum AVPixelFormat pixfmt)
{
    uint8_t* src = NULL;
    uint8_t* p_src = NULL;
    uint8_t* dest = NULL;
    uint32_t height = 0;

    src = xcam_video_buffer_map (img);
    p_src = src;
    if (!src) {
        return;
    }
    height = img->info.height;
    for (uint32_t index = 0; index < img->info.components; index++) {
        src += img->info.offsets[index];
        p_src = src;
        dest = frame->data[index];

        if (img->info.format == V4L2_PIX_FMT_NV12) {
            height = height >> index;
        }
        for (uint32_t i = 0; i < height; i++) {
            memcpy (dest, p_src, frame->linesize[index]);
            p_src += img->info.strides[index];
            dest += frame->linesize[index];
        }
    }
    xcam_video_buffer_unmap (img);
}

static int config_input(AVFilterLink *link)
{
    XCAMContext *s = link->dst->priv;
    char* p_params = NULL;
    char input_params[XCAM_MAX_PARAM_LENGTH] = { 0 };
    int index = 0;
    char* field[XCAM_MAX_PARAMS_COUNT] = { NULL };
    char* value[XCAM_MAX_PARAMS_COUNT] = { NULL };
    char input_width[XCAM_MAX_PARAM_LENGTH] = { 0 };
    char input_height[XCAM_MAX_PARAM_LENGTH] = { 0 };

    s->w = link->w;
    s->h = link->h;
    snprintf(input_width, XCAM_MAX_PARAM_LENGTH - 1, "%d", link->w);
    snprintf(input_height, XCAM_MAX_PARAM_LENGTH - 1, "%d", link->h);

    for (int i = 0; i < FF_ARRAY_ELEMS(xcam_filter_entries); i++) {
        const XCAMFilterEntry *entry = &xcam_filter_entries[i];
        if (!strcmp(s->name, entry->name)) {
            if (s->params) {
                if (strlen(s->params) < XCAM_MAX_PARAM_LENGTH) {
                    strncpy(input_params, s->params, strlen(s->params));
                } else {
                    strncpy(input_params, s->params, XCAM_MAX_PARAM_LENGTH - 1);
                }

                p_params = strtok(input_params, " ");
                while (p_params && index < XCAM_MAX_PARAMS_COUNT) {
                    field[index] = p_params;

                    p_params = strtok(NULL, " ");
                    if (!p_params) break;
                    value[index] = p_params;
                    index++;

                    p_params = strtok(NULL, " ");
                }

                switch (entry->id) {
                case XCAM_3DNR:
                case XCAM_WAVELETNR:
                case XCAM_FISHEYE:
                case XCAM_DEFOG:
                case XCAM_DVS:
                case XCAM_STITCH:
                    xcam_handle_set_parameters (s->handle, "alloc-out-buf", "true", "width", input_width, "height", input_height, field[0], value[0], NULL);
                    break;
                default:
                    return AVERROR(EINVAL);
                    break;
                }
            }else {
                xcam_handle_set_parameters (s->handle, "alloc-out-buf", "true", "width", input_width, "height", input_height, NULL);
            }
            if (XCAM_RETURN_NO_ERROR == xcam_handle_init(s->handle)) {
                return 0;
            } else {
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

static int config_output(AVFilterLink *link)
{
    XCAMContext *s = link->src->priv;

    link->w = s->w;
    if (!strcmp(s->name, "Stitch")) {
        link->h = s->w / 2;
    } else {
        link->h = s->h;
    }
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_NV12, AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static av_cold int init(AVFilterContext *ctx)
{
    XCAMContext *s = ctx->priv;
    if (!s->name) {
        av_log(ctx, AV_LOG_ERROR, "No libxcam filter name specified\n");
        return AVERROR(EINVAL);
    }

    s->handle = xcam_create_handle (s->name);
    if (!s->handle) {
        av_log(ctx, AV_LOG_ERROR, "Create libxcam handle failed\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    XCAMContext *s = ctx->priv;

    xcam_destroy_handle(s->handle);
    s->handle = NULL;

    if (s->uninit)
        s->uninit(ctx);
    av_freep(&s->priv);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    XCamReturn ret;
    XCamVideoFilterBuf buf_in ;
    XCamVideoBuffer* buf_out = NULL;
    AVFilterContext* ctx;
    XCAMContext *s;
    AVFilterLink* outlink;
    AVFrame* out;

    ctx = inlink->dst;
    s = ctx->priv;

    if (!s->handle) {
        av_log(ctx, AV_LOG_ERROR, "Create libxcam handle failed\n");
        return AVERROR(EINVAL);
    }

    ret = xcam_video_buffer_info_reset (
        &buf_in.buf.info, V4L2_PIX_FMT_NV12, in->width, in->height,
        in->linesize[0], in->height, 0);

    if (XCAM_RETURN_NO_ERROR != ret) {
        return AVERROR(ENOMEM);
    }

    buf_in.buf.info.offsets[0] = 0;
    buf_in.buf.info.offsets[1] = (in->data[1]) - (in->data[0]);

    fill_xcam_buffer_from_frame(&buf_in, in, inlink->format);

    ret = xcam_handle_execute (s->handle, (XCamVideoBuffer*)(&buf_in), &buf_out);
    if (XCAM_RETURN_NO_ERROR != ret) {
        return AVERROR_EXTERNAL;
    }

    outlink = inlink->dst->outputs[0];
    outlink->w = buf_out->info.width;
    outlink->h = buf_out->info.height;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    fill_frame_from_xcam_buffer(out, buf_out, outlink->format);
    xcam_video_buffer_unref (buf_out);

    av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(XCAMContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption xcam_options[] = {
    { "filter_name",   NULL, OFFSET(name),   AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "filter_params", NULL, OFFSET(params), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(xcam);

static const AVFilterPad avfilter_vf_xcam_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_xcam_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_xcam = {
    .name          = "xcam",
    .description   = NULL_IF_CONFIG_SMALL("Apply transform using libxcam."),
    .priv_size     = sizeof(XCAMContext),
    .priv_class    = &xcam_class,
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .inputs        = avfilter_vf_xcam_inputs,
    .outputs       = avfilter_vf_xcam_outputs,
};

