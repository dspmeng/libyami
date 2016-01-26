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

    computeBlockLuma(dst);
    cl_int clStatus;
    cl_mem osdLuma = clCreateBuffer(m_context->m_context,
        CL_MEM_READ_ONLY | CL_MEM_HOST_WRITE_ONLY,
        m_osdLuma.size() * sizeof(float),
        NULL,
        &clStatus);
    if (!checkOclStatus(clStatus, "CreateBuffer") ||
        !checkOclStatus(clEnqueueWriteBuffer(m_context->m_queue,
                            osdLuma,
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
    srcImagePtr.reset(new OclVppCLImage(src, m_display, m_context, srcFormat));
    if (!srcImagePtr->numPlanes()) {
        ERROR("failed to create cl image from src frame");
        return YAMI_FAIL;
    }

    dstFormat.image_channel_order = CL_RG;
    dstFormat.image_channel_data_type = CL_UNORM_INT8;
    dstImagePtr.reset(new OclVppCLImage(dst, m_display, m_context, dstFormat));
    if (!dstImagePtr->numPlanes()) {
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
    if ((CL_SUCCESS != clSetKernelArg(m_kernel, 0, sizeof(cl_mem), &dstImagePtr->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 1, sizeof(cl_mem), &dstImagePtr->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 2, sizeof(cl_mem), &bgImageMem[0]))  ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 3, sizeof(cl_mem), &bgImageMem[1]))  ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 4, sizeof(cl_mem), &srcImagePtr->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 5, sizeof(uint32_t), &crop.x))      ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 6, sizeof(uint32_t), &crop.y))      ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 7, sizeof(uint32_t), &crop.width))  ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 8, sizeof(uint32_t), &crop.height)) ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 9, sizeof(cl_mem), &osdLuma)) ||
        (CL_SUCCESS != clSetKernelArg(m_kernel, 10, sizeof(int), &m_blockWidth))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }

    size_t globalWorkSize[2], localWorkSize[2];
    localWorkSize[0] = 8;
    localWorkSize[1] = 8;
    globalWorkSize[0] = ALIGN_POW2(dst.get()->crop.width, localWorkSize[0] * pixelSize) / pixelSize;
    globalWorkSize[1] = ALIGN_POW2(dst.get()->crop.height, localWorkSize[1] * 2) / 2;
    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, m_kernel, 2, NULL,
        globalWorkSize, localWorkSize, 0, NULL, NULL), "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }

    return status;
}

void OclPostProcessOsd::computeBlockLuma(const SharedPtr<VideoFrame>frame)
{
    if (m_blockCount < (int)frame->crop.width / m_blockWidth) {
        m_blockCount = frame->crop.width / m_blockWidth;
        m_osdLuma.resize(m_blockCount);
    }

    SharedPtr<OclVppRawImage> imagePtr(new OclVppRawImage(frame, m_display));
    uint32_t offsetX = frame->crop.x;
    uint32_t offsetY = frame->crop.y;
    uint32_t x, y, acc;
    uint32_t blockThreshold = m_threshold * m_blockWidth * frame->crop.height;
    for (int i = 0; i < m_blockCount; i++) {
        acc = 0;
        for (y = offsetY; y < offsetY + frame->crop.height; y++) {
            for (x = offsetX; x < offsetX + m_blockWidth; x++) {
                acc += imagePtr->pixel(x, y, 0);
            }
        }
        if (acc <= blockThreshold)
            m_osdLuma[i] = 1.0;
        else
            m_osdLuma[i] = 0.0;
        offsetX += m_blockWidth;
    }
}

const bool OclPostProcessOsd::s_registered =
    VaapiPostProcessFactory::register_<OclPostProcessOsd>(YAMI_VPP_OCL_OSD);

}

