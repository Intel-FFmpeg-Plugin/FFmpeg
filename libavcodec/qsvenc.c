/*
 * Intel MediaSDK QSV encoder utility functions
 *
 * copyright (c) 2013 Yukinori Yamazoe
 * copyright (c) 2015 Anton Khirnov
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

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsv_internal.h"
#include "qsvdec.h"
#include "libavcodec/vaapi_allocator.h"

static int init_video_param(AVCodecContext *avctx, QSVEncContext *q)
{
    const char *ratecontrol_desc;

    float quant;
    int ret;

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0)
        return AVERROR_BUG;
    q->param.mfx.CodecId = ret;

    q->width_align = avctx->codec_id == AV_CODEC_ID_HEVC ? 32 : 16;

    if (avctx->level > 0)
        q->param.mfx.CodecLevel = avctx->level;

    q->param.mfx.CodecProfile       = q->profile;

    q->param.mfx.FrameInfo.FourCC         = MFX_FOURCC_NV12;
    q->param.mfx.FrameInfo.CropX          = 0;
    q->param.mfx.FrameInfo.CropY          = 0;
    q->param.mfx.FrameInfo.CropW          = avctx->width;
    q->param.mfx.FrameInfo.CropH          = avctx->height;
    q->param.mfx.FrameInfo.AspectRatioW   = avctx->sample_aspect_ratio.num;
    q->param.mfx.FrameInfo.AspectRatioH   = avctx->sample_aspect_ratio.den;
    q->param.mfx.FrameInfo.ChromaFormat   = MFX_CHROMAFORMAT_YUV420;
    q->param.mfx.FrameInfo.BitDepthLuma   = 8;
    q->param.mfx.FrameInfo.BitDepthChroma = 8;
    q->param.mfx.FrameInfo.Width          = FFALIGN(avctx->width, q->width_align);

    if (avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
       /* A true field layout (TFF or BFF) is not important here,
          it will specified later during frame encoding. But it is important
          to specify is frame progressive or not because allowed heigh alignment
          does depend by this.
        */
        q->param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_FIELD_TFF;
        q->height_align = 32;
    } else {
        q->param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        q->height_align = 16;
    }
   q->param.mfx.FrameInfo.Height    = FFALIGN(avctx->height, q->height_align);

    if (avctx->framerate.den > 0 && avctx->framerate.num > 0) {
        q->param.mfx.FrameInfo.FrameRateExtN = avctx->framerate.num;
        q->param.mfx.FrameInfo.FrameRateExtD = avctx->framerate.den;
    } else {
        q->param.mfx.FrameInfo.FrameRateExtN  = avctx->time_base.den;
        q->param.mfx.FrameInfo.FrameRateExtD  = avctx->time_base.num;
    }

    if (AV_CODEC_ID_MJPEG == avctx->codec_id) {
        av_log(avctx, AV_LOG_INFO, " Init codec is QSV JPEG encode \n");
        q->param.mfx.Interleaved          = 1;
        q->param.mfx.Quality              = q->quality;;
        q->param.mfx.RestartInterval      = 0;

        return 0;
    }

    q->param.mfx.TargetUsage        = q->preset;
    q->param.mfx.GopPicSize         = FFMAX(0, avctx->gop_size);
    q->param.mfx.GopRefDist         = FFMAX(-1, avctx->max_b_frames) + 1;
    q->param.mfx.GopOptFlag         = avctx->flags & AV_CODEC_FLAG_CLOSED_GOP ?
                                      MFX_GOP_CLOSED : 0;
    q->param.mfx.IdrInterval        = q->idr_interval;
    q->param.mfx.NumSlice           = avctx->slices;
    q->param.mfx.NumRefFrame        = FFMAX(0, avctx->refs);
    q->param.mfx.EncodedOrder       = 0;
    q->param.mfx.BufferSizeInKB     = q->buffer_size/1000;

    if (avctx->flags & AV_CODEC_FLAG_QSCALE) {
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
        ratecontrol_desc = "constant quantization parameter (CQP)";
    } else if (avctx->rc_max_rate == avctx->bit_rate) {
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        ratecontrol_desc = "constant bitrate (CBR)";
    } else if (!avctx->rc_max_rate) {
#if QSV_VERSION_ATLEAST(1,7)
        if (q->look_ahead) {
            q->param.mfx.RateControlMethod = MFX_RATECONTROL_LA;
            ratecontrol_desc = "lookahead (LA)";
        } else
#endif
        {
#if 0
            q->param.mfx.RateControlMethod = MFX_RATECONTROL_AVBR;
            ratecontrol_desc = "average variable bitrate (AVBR)";
#else
            q->param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
            ratecontrol_desc = "variable bitrate (VBR)";
#endif
        }
    } else {
        q->param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
        ratecontrol_desc = "variable bitrate (VBR)";
    }

    //q->param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    //ratecontrol_desc = "variable bitrate (VBR)";
    //av_log(avctx, AV_LOG_VERBOSE, "Using the %s ratecontrol method\n", ratecontrol_desc);
    av_log(avctx, AV_LOG_INFO, "Using the %s ratecontrol method\n", ratecontrol_desc);

    switch (q->param.mfx.RateControlMethod) {
    case MFX_RATECONTROL_CBR:
    case MFX_RATECONTROL_VBR:
        q->param.mfx.InitialDelayInKB = avctx->rc_initial_buffer_occupancy / 1000;
        q->param.mfx.TargetKbps       = avctx->bit_rate / 1000;
        q->param.mfx.MaxKbps          = avctx->rc_max_rate / 1000;
        break;
    case MFX_RATECONTROL_CQP:
        quant = avctx->global_quality / FF_QP2LAMBDA;

        q->param.mfx.QPI = av_clip(quant * fabs(avctx->i_quant_factor) + avctx->i_quant_offset, 0, 51);
        q->param.mfx.QPP = av_clip(quant, 0, 51);
        q->param.mfx.QPB = av_clip(quant * fabs(avctx->b_quant_factor) + avctx->b_quant_offset, 0, 51);

        break;
    case MFX_RATECONTROL_AVBR:
#if QSV_VERSION_ATLEAST(1,7)
    case MFX_RATECONTROL_LA:
#endif
        q->param.mfx.TargetKbps  = avctx->bit_rate / 1000;
        q->param.mfx.Convergence = q->avbr_convergence;
        q->param.mfx.Accuracy    = q->avbr_accuracy;
        break;
    }
    av_log(avctx, AV_LOG_INFO, "codecID=%d, profile=%d, level=%d, targetUsage=%d, max_b_frames=%d,IdrInterval=%d, width=%d, height=%d, gop_size=%d, flags=%x, slices=%d, refs=%d, aW=%d, aH=%d buf_occup=%d, width=%d,height=%d, FrameRateN=%d, FrameRateD=%d, time_den=%d, time_num=%d bit_rate=%d, max_bit_rate=%d\n",  
            q->param.mfx.CodecId,
            q->profile, 
            avctx->level,
            q->preset,
            avctx->max_b_frames,
            q->idr_interval, 
            avctx->width,
            avctx->height,
            avctx->gop_size,
            avctx->flags,
            avctx->slices,
            avctx->refs,
            avctx->sample_aspect_ratio.num,
            avctx->sample_aspect_ratio.den,
            avctx->rc_initial_buffer_occupancy,
            q->param.mfx.FrameInfo.Width,
            q->param.mfx.FrameInfo.Height,
            avctx->framerate.num,
            avctx->framerate.den,
            avctx->time_base.den,
            avctx->time_base.num,
            avctx->bit_rate,
            avctx->rc_max_rate
            );

    // the HEVC encoder plugin currently fails if coding options
    // are provided
    if (avctx->codec_id != AV_CODEC_ID_HEVC) {
        q->extco.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
        q->extco.Header.BufferSz      = sizeof(q->extco);
        q->extco.CAVLC                = avctx->coder_type == FF_CODER_TYPE_VLC ?
                                        MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN;

        q->extco.PicTimingSEI         = q->pic_timing_sei ?
                                        MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN;

#if  1
       q->extco.RateDistortionOpt     = q->rate_distor_opt ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.CAVLC                 = q->cavlc ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.NalHrdConformance     = q->nal_hrd_con ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.SingleSeiNalUnit      = q->single_sei_nal ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.ResetRefList          = q->reset_reflist ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.RefPicMarkRep         = q->ref_pic_mark_rep ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.FieldOutput           = q->field_output ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.MaxDecFrameBuffering  = q->max_dec_frame_buffering ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.AUDelimiter           = q->audelimiter ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.VuiNalHrdParameters   = q->vui_nal_hrd_parameters ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.FramePicture          = q->frame_picture ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
       q->extco.RecoveryPointSEI      = q->recovery_pointSEI ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
#endif
        q->extparam[0] = (mfxExtBuffer *)&q->extco;

#if QSV_VERSION_ATLEAST(1,6)
        q->extco2.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION2;
        q->extco2.Header.BufferSz      = sizeof(q->extco2);

#if QSV_VERSION_ATLEAST(1,7)
        // valid value range is from 10 to 100 inclusive
        // to instruct the encoder to use the default value this should be set to zero
        q->extco2.LookAheadDepth        = q->look_ahead_depth != 0 ? FFMAX(10, q->look_ahead_depth) : 0;
#endif
#if QSV_VERSION_ATLEAST(1,8)
        q->extco2.LookAheadDS           = q->look_ahead_downsampling;
#endif
        if(MFX_RATECONTROL_LA != q->param.mfx.RateControlMethod ){
            q->extco2.MaxQPI                = q->maxQPI;
            q->extco2.MinQPI                = q->minQPI;
            q->extco2.MaxQPP                = q->maxQPP;
            q->extco2.MinQPP                = q->minQPP;
            q->extco2.MaxQPB                = q->maxQPB;
            q->extco2.MinQPB                = q->maxQPB;
            q->extco2.MBBRC                 = q->MBBRC;
        }

#if 1
        //Intra Refresh
        if(q->intref_cyclesize > 2) {
            q->extco2.IntRefType          = 1; //VERT_REFRESH
            q->extco2.IntRefCycleSize     = q->intref_cyclesize;
            q->extco2.IntRefQPDelta       = q->intref_QPdelta;
        }

        if(q->maxframesize) {
           if((MFX_RATECONTROL_AVBR == q->param.mfx.RateControlMethod) ||
              (MFX_RATECONTROL_VBR == q->param.mfx.RateControlMethod))
               q->extco2.MaxFrameSize            = q->maxframesize;
           else
               av_log(avctx, AV_LOG_DEBUG, "MaxFrameSize only used in AVBR and VBR \n");
        }
        q->extco2.MaxSliceSize            = q->maxslicesize;
        q->extco2.Trellis                 = q->trellis;
        q->extco2.RepeatPPS               = q->repeatPPS_off ? MFX_CODINGOPTION_OFF : MFX_CODINGOPTION_ON;
        q->extco2.AdaptiveI               = q->adaptiveI ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        q->extco2.AdaptiveB               = q->adaptiveB ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        q->extco2.NumMbPerSlice           = q->numMb_per_slice;
        q->extco2.FixedFrameRate          = q->fixed_framerate ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        q->extco2.DisableVUI              = q->disable_VUI ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        q->extco2.BufferingPeriodSEI      = q->buffing_periodSEI;
        q->extco2.EnableMAD               = q->enableMAD ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        q->extco2.UseRawRef               = q->use_raw_ref ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
#endif

        q->extco2.BRefType              = q->BRefControl;
        q->extco2.BitrateLimit          = MFX_CODINGOPTION_OFF;
        q->extparam[1] = (mfxExtBuffer *)&q->extco2;


#if 1
        // encode mfxExtCodingOption3
        q->extco3.Header.BufferId         = MFX_EXTBUFF_CODING_OPTION3;
        q->extco3.Header.BufferSz         = sizeof(q->extco3);

        q->extco3.NumSliceI               = q->num_slice_I;

        if((q->param.mfx.RateControlMethod == MFX_RATECONTROL_LA) ||
           (q->param.mfx.RateControlMethod == MFX_RATECONTROL_LA_HRD)) {
           if((q->win_brc_size) && (q->winbrc_maxavg_kbps)) {
              q->extco3.WinBRCMaxAvgKbps  = q->winbrc_maxavg_kbps;
              q->extco3.WinBRCSize        = q->win_brc_size;
            }
        }
        if(q->qvbr_quality >= 1) {
           if(q->param.mfx.RateControlMethod == MFX_RATECONTROL_QVBR)
               q->extco3.QVBRQuality       = q->qvbr_quality;
           else
               av_log(avctx, AV_LOG_DEBUG, "QVBRQuality only used for MFX_RATECONTROL_QVBR \n");
        }

        q->extco3.DirectBiasAdjustment  = q->direct_bias_adj ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;

        if(q->enable_global_motion_bias) {
          q->extco3.GlobalMotionBiasAdjustment    = MFX_CODINGOPTION_ON;
          q->extco3.MVCostScalingFactor           = q->mv_cost_sf;
        }

        q->extparam[2] = (mfxExtBuffer *)&q->extco3;
#endif

#endif
        q->param.ExtParam    = q->extparam;
        q->param.NumExtParam = FF_ARRAY_ELEMS(q->extparam);
    }

    return 0;
}

static int qsv_retrieve_enc_params(AVCodecContext *avctx, QSVEncContext *q)
{
    uint8_t sps_buf[128];
    uint8_t pps_buf[128];
    uint8_t vps_buf[128];

    mfxExtCodingOptionSPSPPS spspps = {
        .Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS,
        .Header.BufferSz = sizeof(spspps),
        .SPSBuffer = sps_buf, .SPSBufSize = sizeof(sps_buf),
        .PPSBuffer = pps_buf, .PPSBufSize = sizeof(pps_buf)
    };

    mfxExtCodingOptionVPS vps = {
        .Header.BufferId = MFX_EXTBUFF_CODING_OPTION_VPS,
        .Header.BufferSz = sizeof(vps),
        .VPSBuffer  = vps_buf, .VPSBufSize = sizeof(vps_buf),
    };

    mfxExtBuffer *ext_buffers[] = {
        (mfxExtBuffer*)&spspps,
    };

    int need_pps = avctx->codec_id != AV_CODEC_ID_MPEG2VIDEO;
    int need_vps = avctx->codec_id == AV_CODEC_ID_HEVC;
    int ret;

    q->param.ExtParam    = ext_buffers;
    q->param.NumExtParam = FF_ARRAY_ELEMS(ext_buffers);

    ret = MFXVideoENCODE_GetVideoParam(q->internal_qs.session, &q->param);
    if (ret < 0)
        return ff_qsv_error(ret);

    q->packet_size = q->param.mfx.BufferSizeInKB * 1000;
    if (0 == q->packet_size) {
        q->packet_size = q->param.mfx.FrameInfo.Height * q->param.mfx.FrameInfo.Width * 4;
    }

    if (need_vps) {
        ext_buffers[0] = (mfxExtBuffer*)&vps;
        ret = MFXVideoENCODE_GetVideoParam(q->internal_qs.session, &q->param);
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING, "VPS is needed but not found.\n");
    }

    if (   !spspps.SPSBufSize
        || (need_pps && !spspps.PPSBufSize)
        /*Note: if no vps found, we can generate a fake one*/
        /*|| (need_vps && !vps.VPSBufSize)*/) {
        av_log(avctx, AV_LOG_ERROR, "No extradata returned from libmfx.\n");
        return AVERROR_UNKNOWN;
    }

    avctx->extradata_size = spspps.SPSBufSize +
                            need_pps * spspps.PPSBufSize +
                            need_vps * vps.VPSBufSize;
    avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);

    if (need_vps && vps.VPSBufSize)
        memcpy(avctx->extradata, vps.VPSBuffer, vps.VPSBufSize);
    memcpy(avctx->extradata + need_vps * vps.VPSBufSize, spspps.SPSBuffer, spspps.SPSBufSize);
    if (need_pps)
        memcpy(avctx->extradata + need_vps * vps.VPSBufSize + spspps.SPSBufSize,
                spspps.PPSBuffer, spspps.PPSBufSize);

    /*
     * For hevc, if hw_plugin is loaded, we can get VPS via API.
     * Otherwise, we set this flag to 0 to generate a fake VPS outside.
     */
    q->has_vps = (need_vps && vps.VPSBufSize);

    return 0;
}

int ff_qsv_enc_init(AVCodecContext *avctx, QSVEncContext *q)
{
    int ret;

    av_log(avctx, AV_LOG_INFO, "ff_qsv_enc_init is here: %p\n", q->session);

    q->param.AsyncDepth = q->async_depth;

    q->async_fifo = av_fifo_alloc((1 + q->async_depth) *
                                  (sizeof(AVPacket) + sizeof(mfxSyncPoint) + sizeof(mfxBitstream*)));
    if (!q->async_fifo)
        return AVERROR(ENOMEM);
#if 0
    if (avctx->hwaccel_context) {
        AVQSVContext *qsv = avctx->hwaccel_context;

        q->session         = qsv->session;
        q->param.IOPattern = qsv->iopattern;
    }
#endif

    if (!q->session) {
        // system  mem create seession with vadisplay handle
        av_log(avctx, AV_LOG_DEBUG, "QSVENC: GPUCopy %s.\n",
                q->internal_qs.gpu_copy == MFX_GPUCOPY_ON ? "enabled":"disabled");
        ret = ff_qsv_init_internal_session(avctx, &q->internal_qs);

        if (ret < 0){
            av_log(avctx, AV_LOG_ERROR,"init internal session return %d\n", ret);
            return ret;
        }
        q->session = q->internal_qs.session;

    } else {
        // video mem create seesion without vadispaly which get from decode
        ret = ff_qsv_init_internal_session_sp(avctx, &q->internal_qs);

        if (ret < 0){
            av_log(avctx, AV_LOG_ERROR,"init internal session return %d\n", ret);
            return ret;
        }

        ret = MFXJoinSession(q->session, q->internal_qs.session);
        av_log(avctx, AV_LOG_ERROR,"MFXJoinSession  return %d\n", ret);
    }

    if (q->load_plugins) {
        ret = ff_qsv_load_plugins(q->internal_qs.session, q->load_plugins);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to load plugins %s, ret = %s\n",
                    q->load_plugins, av_err2str(ret));
            return ret;
        }
    }

    if( q->iopattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY ){
        av_log(avctx, AV_LOG_INFO, "ff_qsv_enc_init::MFX_IOPATTERN_IN_VIDEO_MEMORY\n");
        q->param.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    }else{
        av_log(avctx, AV_LOG_INFO, "ff_qsv_enc_init::MFX_IOPATTERN_IN_SYSTEM_MEMORY\n");
        q->param.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    }

    ret = init_video_param(avctx, q);
    if (ret < 0)
        return ret;

    av_log(avctx, AV_LOG_INFO, "ENCODE QueryIOSurf start: session=%p\n", q->session);

    if (q->iopattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY) {
        VADisplay  va_dpy;

        ret = MFXVideoCORE_GetHandle(q->session,
                  (mfxHandleType)MFX_HANDLE_VA_DISPLAY, (mfxHDL*)&va_dpy);

        q->internal_qs.va_display = va_dpy;
        ret = MFXVideoCORE_SetHandle( q->internal_qs.session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)va_dpy);

        av_log(avctx, AV_LOG_INFO, "Set vadisplay for inter_session \n");
    }


    // frame_allocator for the seesion
    if (q->iopattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY) {
        //release this when closing the encoder
        QSVContext* qsv_ctx = (QSVContext*) av_malloc(sizeof(QSVContext));
        qsv_ctx->internal_qs = q->internal_qs;

	q->inter_allocator.Alloc  = ff_qsv_frame_alloc;
	q->inter_allocator.Lock   = ff_qsv_frame_lock;
	q->inter_allocator.Unlock = ff_qsv_frame_unlock;
	q->inter_allocator.GetHDL = ff_qsv_frame_get_hdl;
	q->inter_allocator.Free   = ff_qsv_frame_free;
	q->inter_allocator.pthis  = qsv_ctx;

        MFXVideoCORE_SetFrameAllocator(q->internal_qs.session, &q->inter_allocator);
    }

    ret = MFXVideoENCODE_QueryIOSurf(q->internal_qs.session, &q->param, &q->req);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error querying the encoding parameters\n");
        return ff_qsv_error(ret);
    }
    av_log(avctx, AV_LOG_INFO, "Encoder request %d surfaces\n", q->req.NumFrameSuggested);


    av_log(avctx, AV_LOG_INFO, "MFXVideoENCODE_Init here\n");
    ret = MFXVideoENCODE_Init(q->internal_qs.session, &q->param);
    if (MFX_WRN_PARTIAL_ACCELERATION==ret) {
        av_log(avctx, AV_LOG_WARNING, "Encoder will work with partial HW acceleration\n");
    } else if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error initializing the encoder: %d\n", ret);
        return ff_qsv_error(ret);
    }

    ret = qsv_retrieve_enc_params(avctx, q);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error retrieving encoding parameters.\n");
        return ret;
    }

    q->avctx = avctx;

    av_log(avctx, AV_LOG_INFO, "ENCODEC: ff_qsv_enc_init is done!\n");

    return 0;
}

static void clear_unused_frames(QSVEncContext *q)
{
    QSVFrame *cur = q->work_frames;
    while (cur) {
        if (cur->surface && !cur->surface->Data.Locked) {
            cur->surface = NULL;
            av_frame_unref(cur->frame);
        }
        cur = cur->next;
    }
}

static int get_free_frame(QSVEncContext *q, QSVFrame **f)
{
    QSVFrame *frame, **last;

    clear_unused_frames(q);

    frame = q->work_frames;
    last  = &q->work_frames;
    while (frame) {
        if (!frame->surface) {
            *f = frame;
            return 0;
        }

        last  = &frame->next;
        frame = frame->next;
    }

    frame = av_mallocz(sizeof(*frame));
    if (!frame)
        return AVERROR(ENOMEM);
    frame->frame = av_frame_alloc();
    if (!frame->frame) {
        av_freep(&frame);
        return AVERROR(ENOMEM);
    }
    *last = frame;

    *f = frame;

    return 0;
}

static int submit_frame_videomem(QSVEncContext *q, const AVFrame *frame,
                        mfxFrameSurface1 **surface)
{
    if( frame->data[3]!=NULL )
    {
        *surface = (mfxFrameSurface1*)frame->data[3];
        //printf("ENCODE: surface MemId=%p Lock=%d\n", (*surface)->Data.MemId, (*surface)->Data.Locked);
        (*surface)->Data.TimeStamp = av_rescale_q(frame->pts, q->avctx->time_base, (AVRational){1, 90000}); 
        return 0;
    }

    return -1;
}

static int submit_frame_sysmem(QSVEncContext *q, const AVFrame *frame,
                        mfxFrameSurface1 **surface)
{
    QSVFrame *qf;
    int ret;

    ret = get_free_frame(q, &qf);
    if (ret < 0)
        return ret;

    if (frame->format == AV_PIX_FMT_QSV) {
        ret = av_frame_ref(qf->frame, frame);
        if (ret < 0)
            return ret;

        qf->surface = (mfxFrameSurface1*)qf->frame->data[3];
        *surface = qf->surface;
        return 0;
    }

    /* make a copy if the input is not padded as libmfx requires */
    if (     frame->height & (q->height_align - 1) ||
        frame->linesize[0] & (q->width_align - 1)) {
        qf->frame->height = FFALIGN(frame->height, q->height_align);
        qf->frame->width  = FFALIGN(frame->width, q->width_align);

        ret = ff_get_buffer(q->avctx, qf->frame, AV_GET_BUFFER_FLAG_REF);
        if (ret < 0)
            return ret;

        qf->frame->height = frame->height;
        qf->frame->width  = frame->width;
        ret = av_frame_copy(qf->frame, frame);
        if (ret < 0) {
            av_frame_unref(qf->frame);
            return ret;
        }
    } else {
        ret = av_frame_ref(qf->frame, frame);
        if (ret < 0)
            return ret;
    }

    qf->surface_internal.Info = q->param.mfx.FrameInfo;

    qf->surface_internal.Info.PicStruct =
        !frame->interlaced_frame ? MFX_PICSTRUCT_PROGRESSIVE :
        frame->top_field_first   ? MFX_PICSTRUCT_FIELD_TFF :
                                   MFX_PICSTRUCT_FIELD_BFF;
    if (frame->repeat_pict == 1)
        qf->surface_internal.Info.PicStruct |= MFX_PICSTRUCT_FIELD_REPEATED;
    else if (frame->repeat_pict == 2)
        qf->surface_internal.Info.PicStruct |= MFX_PICSTRUCT_FRAME_DOUBLING;
    else if (frame->repeat_pict == 4)
        qf->surface_internal.Info.PicStruct |= MFX_PICSTRUCT_FRAME_TRIPLING;

    qf->surface_internal.Data.PitchLow  = qf->frame->linesize[0];
    qf->surface_internal.Data.Y         = qf->frame->data[0];
    qf->surface_internal.Data.UV        = qf->frame->data[1];
    qf->surface_internal.Data.TimeStamp = av_rescale_q(frame->pts, q->avctx->time_base, (AVRational){1, 90000});

    qf->surface = &qf->surface_internal;

    *surface = qf->surface;

    return 0;
}


static int submit_frame(QSVEncContext *q, const AVFrame *frame,
                        mfxFrameSurface1 **surface)
{
    if( q->iopattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY ){
        return submit_frame_videomem(q, frame, surface);
    }else{
        return submit_frame_sysmem(q, frame, surface);
    }

    return 0;
}

static void print_interlace_msg(AVCodecContext *avctx, QSVEncContext *q)
{
    if (q->param.mfx.CodecId == MFX_CODEC_AVC) {
        if (q->param.mfx.CodecProfile == MFX_PROFILE_AVC_BASELINE ||
            q->param.mfx.CodecLevel < MFX_LEVEL_AVC_21 ||
            q->param.mfx.CodecLevel > MFX_LEVEL_AVC_41)
            av_log(avctx, AV_LOG_WARNING,
                   "Interlaced coding is supported"
                   " at Main/High Profile Level 2.1-4.1\n");
    }
}

int ff_qsv_encode(AVCodecContext *avctx, QSVEncContext *q,
                  AVPacket *pkt, const AVFrame *frame, int *got_packet)
{
    AVPacket new_pkt = { 0 };
    mfxBitstream *bs;
    mfxFrameSurface1 *surf = NULL;
    mfxSyncPoint sync      = NULL;
    int ret;
    mfxEncodeCtrl ctrl = {{0}};

    if (frame) {
        ret = submit_frame(q, frame, &surf);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error submitting the frame for encoding.\n");
            return ret;
        }
        if (frame->pict_type == AV_PICTURE_TYPE_I) {
            ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF;
            if (q->force_idr)
                ctrl.FrameType |= MFX_FRAMETYPE_IDR;
        }
    }

    ret = av_new_packet(&new_pkt, q->packet_size);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error allocating the output packet\n");
        return ret;
    }

    bs = av_mallocz(sizeof(*bs));
    if (!bs) {
        av_packet_unref(&new_pkt);
        return AVERROR(ENOMEM);
    }
    bs->Data      = new_pkt.data;
    bs->MaxLength = new_pkt.size;
    do {
        ret = MFXVideoENCODE_EncodeFrameAsync(q->internal_qs.session, &ctrl, surf, bs, &sync);
        if (ret == MFX_WRN_DEVICE_BUSY) {
            av_usleep(500);
            continue;
        }
#if 0
        printf("MFXVideoENCODE_EncodeFrameAsync surf=%p sync=%d, ret=%d\n",surf->Data.MemId, sync, ret);
        printf("DECODE: surface info: width=%d height=%d %d %d %d %d\n", 
                        surf->Info.Width, 
                        surf->Info.Height, 
                        surf->Info.CropX, 
                        surf->Info.CropY, 
                        surf->Info.CropW, 
                        surf->Info.CropH);
        printf("DECODE: surface info: FourCC=%d ChromaFormat=%d\n", 
                        (surf->Info.FourCC!=MFX_FOURCC_NV12)?1:0, 
                        (surf->Info.ChromaFormat!=MFX_CHROMAFORMAT_YUV420)?1:0);
        printf("DECODE: surface info: BitDepthLuma=%d, BitDepthChroma=%d, Shift=%d\n",
                        surf->Info.BitDepthLuma, 
                        surf->Info.BitDepthChroma, 
                        surf->Info.Shift); 
#endif
        break;
    } while ( 1 );

    if (ret < 0) {
        av_packet_unref(&new_pkt);
        av_freep(&bs);
        if (ret == MFX_ERR_MORE_DATA)
            return 0;
        av_log(avctx, AV_LOG_ERROR, "EncodeFrameAsync returned %d\n", ret);
        return ff_qsv_error(ret);
    }

    if (ret == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
        if (frame->interlaced_frame)
            print_interlace_msg(avctx, q);
        else
            av_log(avctx, AV_LOG_WARNING,
                   "EncodeFrameAsync returned 'incompatible param' code\n");
    }
    if (sync) {
        av_fifo_generic_write(q->async_fifo, &new_pkt, sizeof(new_pkt), NULL);
        av_fifo_generic_write(q->async_fifo, &sync,    sizeof(sync),    NULL);
        av_fifo_generic_write(q->async_fifo, &bs,      sizeof(bs),    NULL);
    } else {
        av_packet_unref(&new_pkt);
        av_freep(&bs);
    }

    if (av_fifo_size(q->async_fifo) ||
        (!frame && av_fifo_size(q->async_fifo))) {
        av_fifo_generic_read(q->async_fifo, &new_pkt, sizeof(new_pkt), NULL);
        av_fifo_generic_read(q->async_fifo, &sync,    sizeof(sync),    NULL);
        av_fifo_generic_read(q->async_fifo, &bs,      sizeof(bs),      NULL);

        MFXVideoCORE_SyncOperation(q->internal_qs.session, sync, 60000);

        new_pkt.dts  = av_rescale_q(bs->DecodeTimeStamp, (AVRational){1, 90000}, avctx->time_base);
        new_pkt.pts  = av_rescale_q(bs->TimeStamp,       (AVRational){1, 90000}, avctx->time_base);
        new_pkt.size = bs->DataLength;

        if (bs->FrameType & MFX_FRAMETYPE_IDR ||
            bs->FrameType & MFX_FRAMETYPE_xIDR)
            new_pkt.flags |= AV_PKT_FLAG_KEY;

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        if (bs->FrameType & MFX_FRAMETYPE_I || bs->FrameType & MFX_FRAMETYPE_xI)
            avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
        else if (bs->FrameType & MFX_FRAMETYPE_P || bs->FrameType & MFX_FRAMETYPE_xP)
            avctx->coded_frame->pict_type = AV_PICTURE_TYPE_P;
        else if (bs->FrameType & MFX_FRAMETYPE_B || bs->FrameType & MFX_FRAMETYPE_xB)
            avctx->coded_frame->pict_type = AV_PICTURE_TYPE_B;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        av_freep(&bs);

        if (pkt->data) {
            if (pkt->size < new_pkt.size) {
                av_log(avctx, AV_LOG_ERROR, "Submitted buffer not large enough: %d < %d\n",
                       pkt->size, new_pkt.size);
                av_packet_unref(&new_pkt);
                return AVERROR(EINVAL);
            }

            memcpy(pkt->data, new_pkt.data, new_pkt.size);
            pkt->size = new_pkt.size;

            ret = av_packet_copy_props(pkt, &new_pkt);
            av_packet_unref(&new_pkt);
            if (ret < 0)
                return ret;
        } else
            *pkt = new_pkt;

        *got_packet = 1;
    }

    return 0;
}

int ff_qsv_enc_close(AVCodecContext *avctx, QSVEncContext *q)
{
    QSVFrame *cur;

    MFXVideoENCODE_Close(q->session);
    q->session = NULL;

    ff_qsv_close_internal_session(&q->internal_qs);

    //free in decoder if video memory is used
    if( q->iopattern != MFX_IOPATTERN_OUT_VIDEO_MEMORY ){
        cur = q->work_frames;
        while (cur) {
            q->work_frames = cur->next;
            av_frame_free(&cur->frame);
            av_freep(&cur);
            cur = q->work_frames;
        }
    }
    while (q->async_fifo && av_fifo_size(q->async_fifo)) {
        AVPacket pkt;
        mfxSyncPoint sync;
        mfxBitstream *bs;

        av_fifo_generic_read(q->async_fifo, &pkt,  sizeof(pkt),  NULL);
        av_fifo_generic_read(q->async_fifo, &sync, sizeof(sync), NULL);
        av_fifo_generic_read(q->async_fifo, &bs,   sizeof(bs),   NULL);

        av_freep(&bs);
        av_packet_unref(&pkt);
    }
    av_fifo_free(q->async_fifo);
    q->async_fifo = NULL;

    return 0;
}
