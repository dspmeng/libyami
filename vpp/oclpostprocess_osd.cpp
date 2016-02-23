/*
 *  oclpostprocess_osd.cpp - opencl based OSD of contrastive font color
 *
 *  Copyright (C) 2016 Intel Corporation
 *    Author: Jia Meng<jia.meng@intel.com>
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

#include "oclpostprocess_osd.h"
#include "vaapipostprocess_factory.h"
#include "common/common_def.h"
#include "common/log.h"
#include "ocl/oclcontext.h"
#include "vpp/oclvppimage.h"

namespace YamiMediaCodec{

YamiStatus
OclPostProcessOsd::process(const SharedPtr<VideoFrame>& src,
                           const SharedPtr<VideoFrame>& dst)
{
    YamiStatus status = ensureContext("osd");
    if (status != YAMI_SUCCESS)
        return status;

    if (src->fourcc != YAMI_FOURCC_RGBA || dst->fourcc != YAMI_FOURCC_NV12) {
        ERROR("only support RGBA OSD on NV12 video frame");
        return YAMI_INVALID_PARAM;
    }

    status = computeBlockLuma(dst);
    if (status != YAMI_SUCCESS)
        return status;

    cl_int clStatus;
    cl_mem clBuf = clCreateBuffer(m_context->m_context,
        CL_MEM_READ_ONLY | CL_MEM_HOST_WRITE_ONLY,
        m_osdLuma.size() * sizeof(float),
        NULL,
        &clStatus);
    SharedPtr<cl_mem> osdLuma(new cl_mem(clBuf), OclMemDeleter());
    if (!checkOclStatus(clStatus, "CreateBuffer") ||
        !checkOclStatus(clEnqueueWriteBuffer(m_context->m_queue,
                            *osdLuma,
                            CL_TRUE,
                            0,
                            m_osdLuma.size() * sizeof(float),
                            m_osdLuma.data(),
                            0, NULL, NULL),
                        "EnqueueWriteBuffer")) {
        return YAMI_FAIL;
    }

    SharedPtr<OclVppCLImage> srcImagePtr, dstImagePtr;
    cl_image_format srcFormat, dstFormat;

    srcFormat.image_channel_order = CL_RGBA;
    srcFormat.image_channel_data_type = CL_UNORM_INT8;
    srcImagePtr = OclVppCLImage::create(m_display, src, m_context, srcFormat);
    if (!srcImagePtr) {
        ERROR("failed to create cl image from src frame");
        return YAMI_FAIL;
    }

    dstFormat.image_channel_order = CL_RG;
    dstFormat.image_channel_data_type = CL_UNORM_INT8;
    dstImagePtr = OclVppCLImage::create(m_display, dst, m_context, dstFormat);
    if (!dstImagePtr) {
        ERROR("failed to create cl image from dst frame");
        return YAMI_FAIL;
    }

    cl_mem bgImageMem[3];
    for (uint32_t n = 0; n < dstImagePtr->numPlanes(); n++) {
        bgImageMem[n] = dstImagePtr->plane(n);
    }

    uint32_t pixelSize = getPixelSize(dstFormat);
    VideoRect crop;
    crop.x = dst->crop.x / pixelSize;
    crop.y = dst->crop.y & ~1;
    crop.width = dst->crop.width / pixelSize;
    crop.height = dst->crop.height;
    cl_kernel kernel = getKernel("osd");
    if (!kernel) {
        ERROR("failed to get cl kernel");
        return YAMI_FAIL;
    }
    if ((CL_SUCCESS != clSetKernelArg(kernel, 0, sizeof(cl_mem), &dstImagePtr->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 1, sizeof(cl_mem), &dstImagePtr->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 2, sizeof(cl_mem), &bgImageMem[0]))  ||
        (CL_SUCCESS != clSetKernelArg(kernel, 3, sizeof(cl_mem), &bgImageMem[1]))  ||
        (CL_SUCCESS != clSetKernelArg(kernel, 4, sizeof(cl_mem), &srcImagePtr->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 5, sizeof(uint32_t), &crop.x))      ||
        (CL_SUCCESS != clSetKernelArg(kernel, 6, sizeof(uint32_t), &crop.y))      ||
        (CL_SUCCESS != clSetKernelArg(kernel, 7, sizeof(uint32_t), &crop.width))  ||
        (CL_SUCCESS != clSetKernelArg(kernel, 8, sizeof(uint32_t), &crop.height)) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 9, sizeof(cl_mem), osdLuma.get())) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 10, sizeof(int), &m_blockWidth))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }

    size_t globalWorkSize[2], localWorkSize[2];
    localWorkSize[0] = 8;
    localWorkSize[1] = 8;
    globalWorkSize[0] = ALIGN_POW2(dst->crop.width, localWorkSize[0] * pixelSize) / pixelSize;
    globalWorkSize[1] = ALIGN_POW2(dst->crop.height, localWorkSize[1] * 2) / 2;
    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, kernel, 2, NULL,
        globalWorkSize, localWorkSize, 0, NULL, NULL), "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }

    return status;
}

YamiStatus OclPostProcessOsd::setParameters(VppParamType type, void* vppParam)
{
    YamiStatus status = YAMI_INVALID_PARAM;

    switch(type) {
    case VppParamTypeOsd: {
            VppParamOsd* osd = (VppParamOsd*)vppParam;
            if (osd->size == sizeof(VppParamOsd)) {
                m_blockWidth = osd->blockWidth;
                m_threshold = osd->threshold;
                status = YAMI_SUCCESS;
            }
        }
        break;
    default:
        status = OclPostProcessBase::setParameters(type, vppParam);
        break;
    }
    return status;
}

YamiStatus OclPostProcessOsd::computeBlockLuma(const SharedPtr<VideoFrame>frame)
{
    cl_image_format format;
    format.image_channel_order = CL_RGBA;
    format.image_channel_data_type = CL_UNSIGNED_INT8;
    uint32_t pixelSize = getPixelSize(format);

    if (m_blockCount < (int)(frame->crop.width / m_blockWidth)) {
        m_blockCount = frame->crop.width / m_blockWidth;
        m_osdLuma.resize(m_blockCount);
    }

    uint32_t padding = frame->crop.x % pixelSize;
    uint32_t alignedWidth = frame->crop.width + padding;
    if (m_lineBuf.size() < alignedWidth)
        m_lineBuf.resize(alignedWidth);

    cl_int clStatus;
    cl_mem clBuf = clCreateBuffer(m_context->m_context,
        CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY,
        m_lineBuf.size() * sizeof(uint32_t),
        NULL,
        &clStatus);
    if (!checkOclStatus(clStatus, "CreateBuffer"))
        return YAMI_FAIL;
    SharedPtr<cl_mem> lineBuf(new cl_mem(clBuf), OclMemDeleter());

    SharedPtr<OclVppCLImage> imagePtr;
    imagePtr = OclVppCLImage::create(m_display, frame, m_context, format);
    if (!imagePtr) {
        ERROR("failed to create cl image from src frame");
        return YAMI_FAIL;
    }

    VideoRect crop;
    crop.x = frame->crop.x / pixelSize;
    crop.y = frame->crop.y;
    crop.width = alignedWidth / pixelSize;
    crop.height = frame->crop.height;
    cl_kernel kernel = getKernel("reduce_luma");
    if (!kernel) {
        ERROR("failed to get cl kernel");
        return YAMI_FAIL;
    }
    if ((CL_SUCCESS != clSetKernelArg(kernel, 0, sizeof(cl_mem), &imagePtr->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 1, sizeof(uint32_t), &crop.x))      ||
        (CL_SUCCESS != clSetKernelArg(kernel, 2, sizeof(uint32_t), &crop.y))      ||
        (CL_SUCCESS != clSetKernelArg(kernel, 3, sizeof(uint32_t), &crop.height)) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 4, sizeof(cl_mem), lineBuf.get()))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }
    size_t localWorkSize = 16;
    size_t globalWorkSize = ALIGN_POW2(alignedWidth, pixelSize * localWorkSize) / pixelSize;
    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, kernel, 1, NULL,
        &globalWorkSize, &localWorkSize, 0, NULL, NULL), "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }
    if (!checkOclStatus(clEnqueueReadBuffer(m_context->m_queue,
                            *lineBuf,
                            CL_TRUE,
                            0,
                            m_lineBuf.size() * sizeof(uint32_t),
                            m_lineBuf.data(),
                            0, NULL, NULL),
                        "EnqueueReadBuffer")) {
        return YAMI_FAIL;
    }

    uint32_t acc;
    int offset;
    uint32_t blockThreshold = m_threshold * m_blockWidth * frame->crop.height;
    for (int i = 0; i < m_blockCount; i++) {
        acc = 0;
        offset = i * m_blockWidth + padding;
        for (uint32_t j = 0; j < m_blockWidth; j++) {
            acc += m_lineBuf[offset + j];
        }
        if (acc <= blockThreshold)
            m_osdLuma[i] = 1.0;
        else
            m_osdLuma[i] = 0.0;
    }

    return YAMI_SUCCESS;
}

const bool OclPostProcessOsd::s_registered =
    VaapiPostProcessFactory::register_<OclPostProcessOsd>(YAMI_VPP_OCL_OSD);

}
