/*
 * Intel MediaSDK QSV based H.264 enccoder
 *
 * copyright (c) 2013 Yukinori Yamazoe
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


#include <stdint.h>
#include <sys/types.h>

#include <mfx/mfxvideo.h>

#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"
#include "h264.h"
#include "qsv.h"
#include "qsv_internal.h"
#include "qsvenc.h"

static av_cold int qsv_enc_init(AVCodecContext *avctx)
{
    QSVH264EncContext *q = avctx->priv_data;

    return ff_qsv_enc_init(avctx, &q->qsv);
}

static int qsv_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                         const AVFrame *frame, int *got_packet)
{
    QSVH264EncContext *q = avctx->priv_data;

    return ff_qsv_encode(avctx, &q->qsv, pkt, frame, got_packet);
}

static av_cold int qsv_enc_close(AVCodecContext *avctx)
{
    QSVH264EncContext *q = avctx->priv_data;

    return ff_qsv_enc_close(avctx, &q->qsv);
}

#define OFFSET(x) offsetof(QSVH264EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Maximum processing parallelism", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VE },
    { "idr_interval", "Distance (in I-frames) between IDR frames", OFFSET(qsv.idr_interval), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "buffer_size", "BufferSizeInKB represents the maximum possible size of any compressed frames", OFFSET(qsv.buffer_size), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "avbr_accuracy",    "Accuracy of the AVBR ratecontrol",    OFFSET(qsv.avbr_accuracy),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "avbr_convergence", "Convergence of the AVBR ratecontrol", OFFSET(qsv.avbr_convergence), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "pic_timing_sei",    "Insert picture timing SEI with pic_struct_syntax element", OFFSET(qsv.pic_timing_sei), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, VE },

    { "maxQPI", "maxumum allowed QP value for I frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.maxQPI), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "minQPI", "minimum allowed QP value for I frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.minQPI), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "maxQPP", "maxumum allowed QP value for P frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.maxQPP), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "minQPP", "minimum allowed QP value for P frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.minQPP), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "maxQPB", "maxumum allowed QP value for B frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.maxQPB), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "minQPB", "minimum allowed QP value for B frame, valid range: 1-51; 0 is default value, no limitation on QP;cannot work with LA.", OFFSET(qsv.minQPB), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },

	{ "MBBRC", "Setting this flag enables macroblock level bitrate control that generally improves subjective visual quality; cannot work with LA.", OFFSET(qsv.MBBRC), AV_OPT_TYPE_INT, { .i64 = MFX_CODINGOPTION_UNKNOWN }, 0, INT_MAX, VE, "MBBRC"  },
	{ "unknown",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_CODINGOPTION_UNKNOWN }, INT_MIN, INT_MAX,     VE, "MBBRC" },
    { "on",         NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_CODINGOPTION_ON      }, INT_MIN, INT_MAX,     VE, "MBBRC" },
    { "off",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_CODINGOPTION_OFF     }, INT_MIN, INT_MAX,     VE, "MBBRC" },


	{ "BRefControl", "BRefControl is used to control usage of B frames as reference in AVC encoder; value: unkown, bRefOff, bRefPyramid", OFFSET(qsv.BRefControl), AV_OPT_TYPE_INT, { .i64 = MFX_B_REF_UNKNOWN }, 0, INT_MAX, VE, "BRefControl" },
	{ "unknown" ,    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_B_REF_UNKNOWN }, INT_MIN, INT_MAX,     VE, "BRefControl" },
    { "bRefOff",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_B_REF_OFF     }, INT_MIN, INT_MAX,     VE, "BRefControl" },
    { "bRefPyramid", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_B_REF_PYRAMID }, INT_MIN, INT_MAX,     VE, "BRefControl" },

#if QSV_VERSION_ATLEAST(1,7)
    { "look_ahead",       "Use VBR algorithm with look ahead",    OFFSET(qsv.look_ahead),       AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "look_ahead_depth", "Depth of look ahead in number frames", OFFSET(qsv.look_ahead_depth), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 100, VE },
#endif

#if QSV_VERSION_ATLEAST(1,8)
    { "look_ahead_downsampling", NULL, OFFSET(qsv.look_ahead_downsampling), AV_OPT_TYPE_INT, { .i64 = MFX_LOOKAHEAD_DS_UNKNOWN }, MFX_LOOKAHEAD_DS_UNKNOWN, MFX_LOOKAHEAD_DS_2x, VE, "look_ahead_downsampling" },
    { "unknown"                , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LOOKAHEAD_DS_UNKNOWN }, INT_MIN, INT_MAX,     VE, "look_ahead_downsampling" },
    { "off"                    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LOOKAHEAD_DS_OFF     }, INT_MIN, INT_MAX,     VE, "look_ahead_downsampling" },
    { "2x"                     , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LOOKAHEAD_DS_2x      }, INT_MIN, INT_MAX,     VE, "look_ahead_downsampling" },
#endif

    { "profile", NULL, OFFSET(qsv.profile), AV_OPT_TYPE_INT, { .i64 = MFX_PROFILE_UNKNOWN }, 0, INT_MAX, VE, "profile" },
    { "unknown" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_UNKNOWN      }, INT_MIN, INT_MAX,     VE, "profile" },
    { "baseline", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_AVC_BASELINE }, INT_MIN, INT_MAX,     VE, "profile" },
    { "main"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_AVC_MAIN     }, INT_MIN, INT_MAX,     VE, "profile" },
    { "high"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_AVC_HIGH     }, INT_MIN, INT_MAX,     VE, "profile" },

    { "preset", NULL, OFFSET(qsv.preset), AV_OPT_TYPE_INT, { .i64 = MFX_TARGETUSAGE_BALANCED }, MFX_TARGETUSAGE_BEST_QUALITY, MFX_TARGETUSAGE_BEST_SPEED,   VE, "preset" },
    { "veryfast",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BEST_SPEED  },   INT_MIN, INT_MAX, VE, "preset" },
    { "faster",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_6  },            INT_MIN, INT_MAX, VE, "preset" },
    { "fast",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_5  },            INT_MIN, INT_MAX, VE, "preset" },
    { "medium",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BALANCED  },     INT_MIN, INT_MAX, VE, "preset" },
    { "slow",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_3  },            INT_MIN, INT_MAX, VE, "preset" },
    { "slower",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_2  },            INT_MIN, INT_MAX, VE, "preset" },
    { "veryslow",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_TARGETUSAGE_BEST_QUALITY  }, INT_MIN, INT_MAX, VE, "preset" },

    { "gpu_copy", "Enable gpu copy in sysmem mode [default = off]", OFFSET(qsv.internal_qs.gpu_copy), AV_OPT_TYPE_INT, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VE, "gpu_copy" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_DEFAULT }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VE, "gpu_copy" },
    { "on", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_ON }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VE, "gpu_copy" },
    { "off", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VE, "gpu_copy" },

    { "ratedisopt",    "Set this flag if rate distortion optimization is needed", OFFSET(qsv.rate_distor_opt), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "cavlc",         "Set, CAVLC is used; if unset, CABAC is used for encoding", OFFSET(qsv.cavlc), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "nalhrdcon",     "Set ON then AVC encoder produces HRD conformant bitstream", OFFSET(qsv.nal_hrd_con), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 1, VE },
    { "singlseinal",   "Set, encoder puts all SEI messages in the singe NAL unit", OFFSET(qsv.single_sei_nal), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "resetreflist",  "Set this flag to reset the reference list to non-IDR I-frames of a GOP sequence", OFFSET(qsv.reset_reflist), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "refpicmarkrep", "Set this flag to write the reference picture marking repetition SEI message into the output bitstream", OFFSET(qsv.ref_pic_mark_rep), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "fieldoutput",   "Set this flag to instruct the AVC encoder to output bitstreams immediately after the encoder encodes a field", OFFSET(qsv.field_output), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "maxdecframebuffering", "Specifies the maximum number of frames buffered in a DPB", OFFSET(qsv.max_dec_frame_buffering), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "audelimiter",    "Set this flag to insert the Access Unit Delimiter NAL", OFFSET(qsv.audelimiter), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "vuinalhrdparam", "Set this flag to insert NAL HRD parameters in the VUI header", OFFSET(qsv.vui_nal_hrd_parameters), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "framepicture",   "Set this flag to encode interlaced fields as interlaced frames", OFFSET(qsv.frame_picture), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "recoverypointSEI", "Set this flag to insert the recovery point SEI message at the beginning of every intra refresh cycle", OFFSET(qsv.recovery_pointSEI), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },

    { "intrefcyclesize",   "Specifies number of pictures within refresh cycle", OFFSET(qsv.intref_cyclesize), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, INT_MAX, VE },
    { "intrefQPdelta",     "Specifies QP difference for inserted intra MBs", OFFSET(qsv.intref_QPdelta), AV_OPT_TYPE_INT, { .i64 = 0 }, -51, 51, VE },
    { "maxframesize",      "Specify maximum encoded frame size in byte used in AVBR/VBR", OFFSET(qsv.maxframesize), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "maxslicesize",      "Specify maximum slice size in bytes", OFFSET(qsv.maxslicesize), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "trellis",           "Used to control trellis quantization in AVC", OFFSET(qsv.trellis), AV_OPT_TYPE_INT, { .i64 = MFX_TRELLIS_UNKNOWN }, MFX_TRELLIS_UNKNOWN, MFX_TRELLIS_I | MFX_TRELLIS_P | MFX_TRELLIS_B, VE },
    { "repeatPPS",         "The default is on and set flag will off the repetition", OFFSET(qsv.repeatPPS_off), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "adaptiveI",         "his flag controls insertion of I frames by the SDK encoder", OFFSET(qsv.adaptiveI), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "adaptiveB",         "This flag controls changing of frame type from B to P", OFFSET(qsv.adaptiveB), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "numMbperslice",     "This option specifies suggested slice size in number of macroblocks", OFFSET(qsv.numMb_per_slice), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "fixedframerate",    "This option sets fixed_frame_rate_flag in VUI", OFFSET(qsv.fixed_framerate), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "disableVUI",        "This option sets fixed_frame_rate_flag in VUI", OFFSET(qsv.disable_VUI), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "bufferPeriodSEI","This option controls insertion of buffering period SEI in the encoded bitstrea", OFFSET(qsv.buffing_periodSEI), AV_OPT_TYPE_INT, { .i64 = MFX_BPSEI_DEFAULT }, MFX_BPSEI_DEFAULT, MFX_BPSEI_IFRAME, VE },
    { "enableMAD",         "Turn ON this flag to enable per-frame reporting of MAD", OFFSET(qsv.enableMAD), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "userawref",         "Set flag to use raw frames for reference instead reconstructed frames", OFFSET(qsv.use_raw_ref), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },

    { "numSlicei",         "The number of slices for I", OFFSET(qsv.num_slice_I), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "winmaxavg",         "Specifies the maximum bitrate averaged over a sliding window for MFX_RATECONTROL_LA/MFX_RATECONTROL_LA_HRD", OFFSET(qsv.winbrc_maxavg_kbps), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "winsize",           "Specifies sliding used for MFX_RATECONTROL_LA/MFX_RATECONTROL_LA_HRD window size in frames", OFFSET(qsv.win_brc_size), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "qvbrquality",       "Specifies quality factor used for MFX_RATECONTROL_QVBR", OFFSET(qsv.qvbr_quality), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 51, VE },
    { "direct_bias_adj",   "Set flag to enable the ENC mode decision algorithm to bias to fewer B Direct/Skip types", OFFSET(qsv.direct_bias_adj), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "glo_motion_bias_adj","Enables global motion bias", OFFSET(qsv.enable_global_motion_bias), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { "mv_cost_sf",         "MV cost scaling ratio", OFFSET(qsv.mv_cost_sf), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 3, VE },

    { "force_idr",          "If forcing key-frames, force them as IDR frames.", OFFSET(qsv.force_idr), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },

    { NULL },
};

static const AVClass class = {
    .class_name = "h264_qsv encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault qsv_enc_defaults[] = {
    { "b",         "1M"    },
    { "refs",      "0"     },
    // same as the x264 default
    { "g",         "250"   },
    { "bf",        "3"     },
    { "coder",     "ac"    },

    { "flags",     "+cgop" },
    { NULL },
};

AVCodec ff_h264_qsv_encoder = {
    .name           = "h264_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVH264EncContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .init           = qsv_enc_init,
    .encode2        = qsv_enc_frame,
    .close          = qsv_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12,
                                                    AV_PIX_FMT_QSV,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = qsv_enc_defaults,
};
