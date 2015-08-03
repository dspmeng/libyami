/*
 *  vaapidecoder_h265.cpp - h265 decoder
 *
 *  Copyright (C) 2013-2014 Intel Corporation
 *    Author: XuGuangxin<Guangxin.Xu@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "codecparsers/h265parser.h"
#include "common/log.h"
#include "common/nalreader.h"
#include "vaapidecoder_factory.h"

#include "vaapi/vaapiptrs.h"
#include "vaapi/vaapicontext.h"
#include "vaapi/vaapidisplay.h"
#include "vaapidecpicture.h"

#include "vaapidecoder_h265.h"




namespace YamiMediaCodec{
typedef VaapiDecoderH265::PicturePtr PicturePtr;

bool isIdr(const H265NalUnit* const nalu)
{
    return nalu->type == H265_NAL_SLICE_IDR_W_RADL
            || nalu->type ==H265_NAL_SLICE_IDR_N_LP;
}

bool isBla(const H265NalUnit* const nalu)
{
    return nalu->type == H265_NAL_SLICE_BLA_W_LP
            || nalu->type == H265_NAL_SLICE_BLA_W_RADL
            || nalu->type == H265_NAL_SLICE_BLA_N_LP;
}

#ifndef H265_NAL_SLICE_RSV_IRAP_VCL23
#define H265_NAL_SLICE_RSV_IRAP_VCL23 23
#endif
bool isIrap(const H265NalUnit* const nalu)
{
    return nalu->type >=  H265_NAL_SLICE_BLA_W_LP
            && nalu->type <= H265_NAL_SLICE_RSV_IRAP_VCL23;
}

bool isRasl(const H265NalUnit* const nalu)
{
    return nalu->type == H265_NAL_SLICE_RASL_R
            || nalu->type == H265_NAL_SLICE_RASL_N;
}

bool isRadl(const H265NalUnit* const nalu)
{
    return nalu->type == H265_NAL_SLICE_RADL_R
            || nalu->type == H265_NAL_SLICE_RADL_N;
}

class VaapiDecPictureH265 : public VaapiDecPicture
{
public:
    VaapiDecPictureH265(const ContextPtr& context, const SurfacePtr& surface, int64_t timeStamp):
        VaapiDecPicture(context, surface, timeStamp), m_poc(0)
    {
    }
    int32_t m_poc;
    bool    m_noRaslOutputFlag;
    bool    m_picOutputFlag;
};
/*
bool VaapiDecoderH265::DPB::init(VaapiDecPictureH265 * picture,
     H265SliceHdr *header,  H265NalUnit * nalu, bool newStream)
{
    const H265PPS *const pps = header->pps;
    const H265SPS *const sps = pps->sps;
    bool noRaslOutputFlag
        = isIdr(nalu) || isBla(nalu) || newStream;
    return true;
}*/

VaapiDecoderH265::VaapiDecoderH265():
    m_prevPicOrderCntMsb(0),
    m_prevPicOrderCntLsb(0),
    m_newStream(true)
{
    m_parser = h265_parser_new();
}

VaapiDecoderH265::~VaapiDecoderH265()
{
    stop();
    h265_parser_free(m_parser);
}

Decode_Status VaapiDecoderH265::start(VideoConfigBuffer * buffer)
{
    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderH265::decodeParamSet(H265NalUnit *nalu)
{
    H265ParserResult result = h265_parser_parse_nal(m_parser, nalu);
    return result == H265_PARSER_OK ? DECODE_SUCCESS : DECODE_FAIL;
}

Decode_Status VaapiDecoderH265::outputPicture(const PicturePtr& picture)
{
    VaapiDecoderBase::PicturePtr base = std::tr1::static_pointer_cast<VaapiDecPicture>(picture);
    return VaapiDecoderBase::outputPicture(base);
}

Decode_Status VaapiDecoderH265::decodeCurrent()
{
    Decode_Status status = DECODE_SUCCESS;
    if (!m_current)
        return status;
    if (!m_current->decode()) {
        ERROR("decode %d failed", m_current->m_poc);
        //ignore it
        return status;
    }
    status = outputPicture(m_current);
    m_current.reset();
    return status;
}


#define FILL_SCALING_LIST(mxm) \
void fillScalingList##mxm(VAIQMatrixBufferHEVC* iqMatrix, const H265ScalingList* scalingList) \
{ \
    for (int i = 0; i < N_ELEMENTS(iqMatrix->ScalingList##mxm); i++) { \
        h265_quant_matrix_##mxm##_get_raster_from_zigzag(iqMatrix->ScalingList##mxm[i], \
            scalingList->scaling_lists_##mxm[i]); \
    } \
}

FILL_SCALING_LIST(4x4)
FILL_SCALING_LIST(8x8)
FILL_SCALING_LIST(16x16)
FILL_SCALING_LIST(32x32)

#define FILL_SCALING_LIST_DC(mxm) \
void fillScalingListDc##mxm(VAIQMatrixBufferHEVC* iqMatrix, const H265ScalingList* scalingList) \
{ \
    for (int i = 0; i < N_ELEMENTS(iqMatrix->ScalingListDC##mxm); i++) { \
        iqMatrix->ScalingListDC##mxm[i] = \
            scalingList->scaling_list_dc_coef_minus8_##mxm[i] + 8; \
    } \
}

FILL_SCALING_LIST_DC(16x16)
FILL_SCALING_LIST_DC(32x32)

bool VaapiDecoderH265::fillIqMatrix(const PicturePtr& picture, const H265SliceHdr* header)
{
    H265PPS* pps = header->pps;
    H265SPS* sps = pps->sps;
    H265ScalingList* scalingList;
    if (pps->scaling_list_data_present_flag) {
        scalingList = &pps->scaling_list;
    } else if (sps->scaling_list_enabled_flag
               && sps->scaling_list_data_present_flag) {
        scalingList = &sps->scaling_list;
    } else {
        //default scaling list
        return true;
    }
    VAIQMatrixBufferHEVC* iqMatrix;
    if (!picture->editIqMatrix(iqMatrix))
        return false;
    fillScalingList4x4(iqMatrix, scalingList);
    fillScalingList8x8(iqMatrix, scalingList);
    fillScalingList16x16(iqMatrix, scalingList);
    fillScalingList32x32(iqMatrix, scalingList);
    fillScalingListDc16x16(iqMatrix, scalingList);
    fillScalingListDc32x32(iqMatrix, scalingList);
    return true;
}

bool VaapiDecoderH265::fillPicture(const PicturePtr& picture, const H265SliceHdr* header)
{
    VAPictureParameterBufferHEVC* param;
    if (!picture->editPicture(param))
        return false;
    param->CurrPic.picture_id = picture->getSurfaceID();
    param->CurrPic.pic_order_cnt = picture->m_poc;
    for (int i = 0; i < N_ELEMENTS(param->ReferenceFrames); i++) {
        param->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    }

    H265PPS* pps = header->pps;
    H265SPS* sps = pps->sps;
#define FILL(h, f) param->f = h->f
    FILL(sps, pic_width_in_luma_samples);
    FILL(sps, pic_height_in_luma_samples);
#define FILL_PIC(h, f) param->pic_fields.bits.f  = h->f
    FILL_PIC(sps, chroma_format_idc);
    FILL_PIC(sps, separate_colour_plane_flag);
    FILL_PIC(sps, pcm_enabled_flag);
    FILL_PIC(sps, scaling_list_enabled_flag);
    FILL_PIC(pps, transform_skip_enabled_flag);
    FILL_PIC(sps, amp_enabled_flag);
    FILL_PIC(sps, strong_intra_smoothing_enabled_flag);
    FILL_PIC(pps, sign_data_hiding_enabled_flag);
    FILL_PIC(pps, constrained_intra_pred_flag);
    FILL_PIC(pps, cu_qp_delta_enabled_flag);
    FILL_PIC(pps, weighted_pred_flag);
    FILL_PIC(pps, weighted_bipred_flag);
    FILL_PIC(pps, transquant_bypass_enabled_flag);
    FILL_PIC(pps, tiles_enabled_flag);
    FILL_PIC(pps, entropy_coding_sync_enabled_flag);
    param->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag
        = pps->loop_filter_across_slices_enabled_flag;
    FILL_PIC(pps, loop_filter_across_tiles_enabled_flag);
    FILL_PIC(sps, pcm_loop_filter_disabled_flag);
    //how to fill this?
    //NoPicReorderingFlag
    //NoBiPredFlag

    param->sps_max_dec_pic_buffering_minus1 =
        sps->max_dec_pic_buffering_minus1[0];
    FILL(sps, bit_depth_luma_minus8);
    FILL(sps, bit_depth_chroma_minus8);
    FILL(sps, pcm_sample_bit_depth_luma_minus1);
    FILL(sps, pcm_sample_bit_depth_chroma_minus1);
    FILL(sps, log2_min_luma_coding_block_size_minus3);
    FILL(sps, log2_diff_max_min_luma_coding_block_size);
    FILL(sps, log2_min_transform_block_size_minus2);
    FILL(sps, log2_diff_max_min_transform_block_size);
    FILL(sps, log2_min_pcm_luma_coding_block_size_minus3);
    FILL(sps, log2_diff_max_min_pcm_luma_coding_block_size);
    FILL(sps, max_transform_hierarchy_depth_intra);
    FILL(sps, max_transform_hierarchy_depth_inter);
    FILL(pps, init_qp_minus26);
    FILL(pps, diff_cu_qp_delta_depth);
    param->pps_cb_qp_offset = pps->cb_qp_offset;
    param->pps_cr_qp_offset = pps->cr_qp_offset;
    FILL(pps, log2_parallel_merge_level_minus2);
    FILL(pps, num_tile_columns_minus1);
    FILL(pps, num_tile_rows_minus1);
#define COPY(f, c) memcpy(param->f, pps->f, (pps->c)*sizeof(pps->f[0]))
    COPY(column_width_minus1, num_tile_columns_minus1);
    COPY(row_height_minus1, num_tile_rows_minus1);


#define FILL_SLICE(h, f)    param->slice_parsing_fields.bits.f = h->f
#define FILL_SLICE_1(h, f) param->slice_parsing_fields.bits.h##_##f = h->f

    FILL_SLICE(pps, lists_modification_present_flag);
    FILL_SLICE(sps, long_term_ref_pics_present_flag);
    FILL_SLICE_1(sps, temporal_mvp_enabled_flag);

    FILL_SLICE(pps, cabac_init_present_flag);
    FILL_SLICE(pps, output_flag_present_flag);
    FILL_SLICE(pps, dependent_slice_segments_enabled_flag);
    FILL_SLICE_1(pps, slice_chroma_qp_offsets_present_flag);
    FILL_SLICE(sps, sample_adaptive_offset_enabled_flag);
    FILL_SLICE(pps, deblocking_filter_override_enabled_flag);
    param->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag =
        pps->deblocking_filter_disabled_flag;
    FILL_SLICE(pps, slice_segment_header_extension_present_flag);

    /* how to fill following fields
    RapPicFlag
    IdrPicFlag
    IntraPicFlag  */

    FILL(sps, log2_max_pic_order_cnt_lsb_minus4);
    FILL(sps, num_short_term_ref_pic_sets);
    param->num_long_term_ref_pic_sps = sps->num_long_term_ref_pics_sps;
    FILL(pps, num_ref_idx_l0_default_active_minus1);
    FILL(pps, num_ref_idx_l1_default_active_minus1);
    param->pps_beta_offset_div2 = pps->beta_offset_div2;
    param->pps_tc_offset_div2 = pps->tc_offset_div2;
    FILL(pps, num_extra_slice_header_bits);

    /* how to fill this
     st_rps_bits*/

#undef FILL
#undef FILL_PIC
#undef FILL_SLICE
#undef FILL_SLICE_1

    return true;
}

bool VaapiDecoderH265::fillReference(const PicturePtr& picture,
        VASliceParameterBufferHEVC* slice, const H265SliceHdr * header)
{
    slice->num_ref_idx_l0_active_minus1 = 0xFF;
    slice->num_ref_idx_l1_active_minus1 = 0xFF;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 15; j++) {
            slice->RefPicList[i][j] = 0xFF;
        }
    }
    return true;

}

inline int32_t clip3(int32_t x, int32_t y, int32_t z)
{
    if (z < x)
        return x;
    if (z > y)
        return y;
    return z;
}

#define FILL_WEIGHT_TABLE(n) \
void fillPredWedightTableL##n(VASliceParameterBufferHEVC* slice, \
        const H265SliceHdr* header, uint8_t chromaLog2WeightDenom) \
{ \
    const H265PredWeightTable& w = header->pred_weight_table; \
    for (int i = 0; i < slice->num_ref_idx_l##n##_active_minus1; i++) { \
        if (w.luma_weight_l##n##_flag[i]) { \
                slice->delta_luma_weight_l##n[i] = w.delta_luma_weight_l##n[i]; \
                slice->luma_offset_l##n[i] = w.luma_offset_l##n[i];\
            } \
            if (w.chroma_weight_l##n##_flag[i]) { \
                for (int j = 0; j < 2; j++) { \
                    int8_t deltaWeight = w.delta_chroma_weight_l##n[i][j]; \
                    int32_t chromaWeight = (1 << chromaLog2WeightDenom) + deltaWeight; \
                    int16_t deltaOffset = w.delta_chroma_offset_l##n[i][j]; \
                    int32_t chromaOffset = \
                        deltaOffset - (((128*chromaWeight)>>chromaLog2WeightDenom) + 128);\
\
                    slice->delta_chroma_weight_l##n[i][j] = deltaWeight; \
                    slice->ChromaOffsetL##n[i][j]= (int8_t)clip3(-128, 127, chromaOffset); \
            } \
        } \
    } \
}

FILL_WEIGHT_TABLE(0)
FILL_WEIGHT_TABLE(1)

bool VaapiDecoderH265::fillPredWeightTable(VASliceParameterBufferHEVC* slice, const H265SliceHdr* header)
{
    H265PPS* pps = header->pps;
    H265SPS* sps = pps->sps;
    const H265PredWeightTable& w = header->pred_weight_table;
    if ((pps->weighted_pred_flag && H265_IS_P_SLICE (header)) ||
            (pps->weighted_bipred_flag && H265_IS_B_SLICE (header))) {
        uint8_t chromaLog2WeightDenom = w.luma_log2_weight_denom;
        slice->luma_log2_weight_denom = w.luma_log2_weight_denom;
        if (sps->chroma_format_idc != 0) {
            slice->delta_chroma_log2_weight_denom
                = w.delta_chroma_log2_weight_denom;
            chromaLog2WeightDenom
                += w.delta_chroma_log2_weight_denom;
        }
        fillPredWedightTableL0(slice,  header, chromaLog2WeightDenom);
        if (pps->weighted_bipred_flag && H265_IS_B_SLICE (header))
            fillPredWedightTableL1(slice,  header, chromaLog2WeightDenom);
    }
    return true;
}

static inline uint32_t
getSliceDataByteOffset(const H265SliceHdr* sliceHdr, uint32_t nalHeaderBytes)
{
    return nalHeaderBytes + (sliceHdr->header_size + 7) / 8
            - sliceHdr->n_emulation_prevention_bytes;
}

bool VaapiDecoderH265::fillSlice(const PicturePtr& picture,
        const H265SliceHdr* header, const H265NalUnit *nalu)
{
    VASliceParameterBufferHEVC* sliceParam;
    if (!picture->newSlice(sliceParam, nalu->data + nalu->offset, nalu->size))
        return false;
    sliceParam->slice_data_byte_offset =
        getSliceDataByteOffset(header, nalu->header_bytes);
    sliceParam->slice_segment_address = header->segment_address;
    if (!fillReference(picture, sliceParam, header))
        return false;

#define FILL_LONG(f) sliceParam->LongSliceFlags.fields.f = header->f
#define FILL_LONG_SLICE(f) sliceParam->LongSliceFlags.fields.slice_##f = header->f
    //how to fill this
    //LastSliceOfPic
    FILL_LONG(dependent_slice_segment_flag);
    FILL_LONG_SLICE(type);
    sliceParam->LongSliceFlags.fields.color_plane_id = header->colour_plane_id;
    FILL_LONG_SLICE(sao_luma_flag);
    FILL_LONG_SLICE(sao_chroma_flag);
    FILL_LONG(mvd_l1_zero_flag);
    FILL_LONG(cabac_init_flag);
    FILL_LONG_SLICE(temporal_mvp_enabled_flag);
    FILL_LONG_SLICE(deblocking_filter_disabled_flag);
    FILL_LONG(collocated_from_l0_flag);
    FILL_LONG_SLICE(loop_filter_across_slices_enabled_flag);

#define FILL(f) sliceParam->f = header->f
#define FILL_SLICE(f) sliceParam->slice_##f = header->f
    FILL(collocated_ref_idx);
    /* following fields fill in fillReference
       num_ref_idx_l0_active_minus1
       num_ref_idx_l1_active_minus1*/

    FILL_SLICE(qp_delta);
    FILL_SLICE(cb_qp_offset);
    FILL_SLICE(cr_qp_offset);
    FILL_SLICE(beta_offset_div2);
    FILL_SLICE(tc_offset_div2);
    if (!fillPredWeightTable(sliceParam, header))
        return false;
    FILL(five_minus_max_num_merge_cand);
    return true;
}

Decode_Status VaapiDecoderH265::ensureContext(const H265SPS* sps)
{
    uint8_t surfaceNumber = sps->max_dec_pic_buffering_minus1[0] + 1 + H265_EXTRA_SURFACE_NUMBER;
    if (m_configBuffer.width != sps->width
        || m_configBuffer.height <  sps->height
        || m_configBuffer.surfaceNumber < surfaceNumber) {
        INFO("frame size changed, reconfig codec. orig size %d x %d, new size: %d x %d",
                m_configBuffer.width, m_configBuffer.height, sps->width, sps->height);
        Decode_Status status = VaapiDecoderBase::terminateVA();
        if (status != DECODE_SUCCESS)
            return status;
        m_configBuffer.width = sps->crop_rect_width ? sps->crop_rect_width : sps->width;
        m_configBuffer.height = sps->crop_rect_height ? sps->crop_rect_height : sps->height;
        m_configBuffer.surfaceWidth = sps->width;
        m_configBuffer.surfaceHeight =sps->height;
        m_configBuffer.flag |= HAS_SURFACE_NUMBER;
        m_configBuffer.profile = VAProfileHEVCMain;
        m_configBuffer.flag &= ~USE_NATIVE_GRAPHIC_BUFFER;
        m_configBuffer.surfaceNumber = surfaceNumber;
        status = VaapiDecoderBase::start(&m_configBuffer);
        if (status != DECODE_SUCCESS)
            return status;
        return DECODE_FORMAT_CHANGE;
    }
    return DECODE_SUCCESS;
}

void VaapiDecoderH265::getPoc(const PicturePtr& picture,
        const H265SliceHdr* const header,
        const H265NalUnit* const nalu)
{
    const H265PPS* const pps = header->pps;
    const H265SPS* const sps = pps->sps;

    uint8_t temporalID = nalu->temporal_id_plus1 - 1;
    //fix me
    ASSERT(!temporalID && "do not support high temporal id ");

    const uint16_t pocLsb = header->pic_order_cnt_lsb;
    const int32_t MaxPicOrderCntLsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    int32_t picOrderCntMsb;
    if (isIrap(nalu) && picture->m_noRaslOutputFlag) {
        picOrderCntMsb = 0;
    } else {
        if((pocLsb < m_prevPicOrderCntMsb)
                && ((m_prevPicOrderCntMsb - pocLsb) >= (MaxPicOrderCntLsb / 2))) {
            picOrderCntMsb = m_prevPicOrderCntMsb + MaxPicOrderCntLsb;
        } else if((pocLsb > m_prevPicOrderCntLsb)
                && ((pocLsb - m_prevPicOrderCntLsb) > (MaxPicOrderCntLsb / 2))) {
            picOrderCntMsb = m_prevPicOrderCntMsb - MaxPicOrderCntLsb;
        } else {
            picOrderCntMsb =  m_prevPicOrderCntMsb;
        }
    }
    picture->m_poc = picOrderCntMsb + pocLsb;
    ERROR("poc = %d", picture->m_poc);
    //fixme:sub-layer non-reference picture.
    if (!isRasl(nalu) || !isRadl(nalu)) {
        m_prevPicOrderCntMsb = picOrderCntMsb;
        m_prevPicOrderCntLsb = pocLsb;
    }
}

PicturePtr VaapiDecoderH265::createPicture(const H265SliceHdr* const header,
        const H265NalUnit* const nalu)
{
    PicturePtr picture;
    SurfacePtr surface = createSurface();
    if (!surface)
        return picture;
    picture.reset(new VaapiDecPictureH265(m_context, surface, m_currentPTS));

    picture->m_noRaslOutputFlag = isIdr(nalu) || isBla(nalu) || m_newStream;
    picture->m_picOutputFlag
        = (isRasl(nalu) && picture->m_noRaslOutputFlag) ? false : header->pic_output_flag;

    getPoc(picture, header, nalu);

    return picture;
}

Decode_Status VaapiDecoderH265::decodeSlice(H265NalUnit *nalu)
{
    H265SliceHdr header;
    H265SliceHdr* slice = &header;
    H265ParserResult result;
    Decode_Status status;
    result = h265_parser_parse_slice_hdr(m_parser, nalu, slice);
    if (result == H265_PARSER_ERROR) {
        return DECODE_FAIL;
    }
    status = ensureContext(slice->pps->sps);
    if (status != DECODE_SUCCESS) {
        return status;
    }
    if (slice->first_slice_segment_in_pic_flag) {
        status = decodeCurrent();
        if (status != DECODE_SUCCESS)
            return status;
        m_current = createPicture(slice, nalu);
        if (!m_current)
            return DECODE_MEMORY_FAIL;
        if (!fillPicture(m_current, slice))
            return DECODE_FAIL;
    }
    if (!m_current)
        return DECODE_FAIL;
    return fillSlice(m_current, slice, nalu);

}

Decode_Status VaapiDecoderH265::decodeNalu(H265NalUnit *nalu)
{
    uint8_t type = nalu->type;
    Decode_Status status = DECODE_SUCCESS;

    if (H265_NAL_SLICE_TRAIL_N <= type && type <= H265_NAL_SLICE_CRA_NUT) {
        status = decodeSlice(nalu);
    } else {
        status = decodeCurrent();
        if (status != DECODE_SUCCESS)
            return status;
        switch (type) {
            case H265_NAL_VPS:
            case H265_NAL_SPS:
            case H265_NAL_PPS:
                status = decodeParamSet(nalu);
                break;
            case H265_NAL_AUD:
            case H265_NAL_EOS:
            case H265_NAL_EOB:
            case H265_NAL_FD:
            case H265_NAL_PREFIX_SEI:
            case H265_NAL_SUFFIX_SEI:
            default:
                break;
        }
    }

    return status;
}

Decode_Status VaapiDecoderH265::decode(VideoDecodeBuffer *buffer)
{
    m_currentPTS = buffer->timeStamp;

    NalReader nr(buffer->data, buffer->size);
    const uint8_t* nal;
    int32_t size;
    Decode_Status status;
    while (nr.read(nal, size)) {
        H265NalUnit nalu;
        if (H265_PARSER_OK == h265_parser_identify_nalu_unchecked(m_parser, nal, 0, size, &nalu)) {
            status = decodeNalu(&nalu);
            if (status != DECODE_SUCCESS)
                return status;
        }
    }
    return DECODE_SUCCESS;
}

const bool VaapiDecoderH265::s_registered =
    VaapiDecoderFactory::register_<VaapiDecoderH265>(YAMI_MIME_H265);

}

