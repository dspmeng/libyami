/*
 *  oclpostprocess_transform.cpp - opencl based flip and rotate
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

#include "oclpostprocess_transform.h"
#include "vaapipostprocess_factory.h"
#include "common/common_def.h"
#include "common/log.h"
#include "ocl/oclcontext.h"
#include "vaapi/vaapiutils.h"
#include "vpp/oclvppimage.h"

namespace YamiMediaCodec{

YamiStatus
OclPostProcessTransform::process(const SharedPtr<VideoFrame>& src,
                                 const SharedPtr<VideoFrame>& dst)
{
    YamiStatus status = ensureContext("transform");
    if (status != YAMI_SUCCESS)
        return status;

    if (src->fourcc != YAMI_FOURCC_NV12 || dst->fourcc != YAMI_FOURCC_NV12) {
        ERROR("only support transform of NV12 video frame");
        return YAMI_INVALID_PARAM;
    }

    cl_image_format format;
    format.image_channel_order = CL_RGBA;
    format.image_channel_data_type = CL_UNORM_INT8;
    SharedPtr<OclVppCLImage> srcImage =
        OclVppCLImage::create(m_display, src, m_context, format);
    if (!srcImage) {
        ERROR("failed to create cl image from src video frame");
        return YAMI_FAIL;
    }
    SharedPtr<OclVppCLImage> dstImage =
        OclVppCLImage::create(m_display, dst, m_context, format);
    if (!dstImage) {
        ERROR("failed to create cl image from dst video frame");
        return YAMI_FAIL;
    }

    if (m_transform & VPP_TRANSFORM_FLIP_H && m_transform & VPP_TRANSFORM_FLIP_V) {
        // same as rotate 180
        m_transform &= ~(VPP_TRANSFORM_FLIP_H | VPP_TRANSFORM_FLIP_V);
        switch (m_transform) {
        case VPP_TRANSFORM_ROT_90:
            m_transform = VPP_TRANSFORM_ROT_270;
            break;
        case VPP_TRANSFORM_ROT_180:
            m_transform = 0;
            break;
        case VPP_TRANSFORM_ROT_270:
            m_transform = VPP_TRANSFORM_ROT_90;
            break;
        default:
            m_transform = VPP_TRANSFORM_ROT_180;
            break;
        }
    }

    bool bFlip = m_transform & (VPP_TRANSFORM_FLIP_H | VPP_TRANSFORM_FLIP_V);
    bool bRotate = m_transform &
        (VPP_TRANSFORM_ROT_90 | VPP_TRANSFORM_ROT_180 | VPP_TRANSFORM_ROT_270);

    if (bFlip && bRotate) {
        uint32_t width = srcImage->getWidth();
        uint32_t height = srcImage->getHeight();
        if (!m_scratchImage ||
            width != m_scratchImage->getWidth() ||
            height != m_scratchImage->getHeight()) {
            m_scratchImage = createScratchImage(width, height);
        }
        if (!m_scratchImage) {
            ERROR("failed to create scratch image");
            return YAMI_FAIL;
        }
        status = flip(srcImage, m_scratchImage);
        if (status != YAMI_SUCCESS) {
            ERROR("failed to flip video frame");
            return status;
        }
        status = rotate(m_scratchImage, dstImage);
        if (status != YAMI_SUCCESS) {
            ERROR("failed to rotate video frame");
            return status;
        }
    } else if (bFlip && !bRotate) {
        status = flip(srcImage, dstImage);
        if (status != YAMI_SUCCESS) {
            ERROR("failed to flip video frame");
            return status;
        }
    } else if (!bFlip && bRotate) {
        status = rotate(srcImage, dstImage);
        if (status != YAMI_SUCCESS) {
            ERROR("failed to rotate video frame");
            return status;
        }
    } else {
        ERROR("neither flip nor rotate is needed");
        return YAMI_INVALID_PARAM;
    }

    return status;
}

YamiStatus OclPostProcessTransform::setParameters(VppParamType type, void* vppParam)
{
    YamiStatus status = YAMI_INVALID_PARAM;

    switch(type) {
    case VppParamTypeTransform: {
            VppParamTransform* param = (VppParamTransform*)vppParam;
            if (param->size == sizeof(VppParamTransform)) {
                m_transform = param->transform;
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

SharedPtr<OclVppCLImage>
OclPostProcessTransform::createScratchImage(uint32_t width, uint32_t height)
{
    SharedPtr<OclVppCLImage> image;
    SharedPtr<VideoFrame> frame;

    VAStatus status;
    VASurfaceID id;
    VASurfaceAttrib attrib;
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = VA_FOURCC_NV12;
    status = vaCreateSurfaces(m_display,
                              VA_RT_FORMAT_YUV420,
                              width,
                              height,
                              &id,
                              1,
                              &attrib,
                              1);
    if (!checkVaapiStatus(status, "VaCreateSurfaces"))
        return image;
    frame.reset(new VideoFrame, VideoFrameDeleter(m_display));
    frame->surface = (intptr_t)id;
    frame->crop.x = frame->crop.y = 0;
    frame->crop.width = width;
    frame->crop.height = height;

    cl_image_format format;
    format.image_channel_order = CL_RGBA;
    format.image_channel_data_type = CL_UNORM_INT8;
    image = OclVppCLImage::create(m_display, frame, m_context, format);
    if (!image) {
        ERROR("failed to create scratch image");
    }
    return image;
}

YamiStatus OclPostProcessTransform::flip(const SharedPtr<OclVppCLImage>& src,
                                         const SharedPtr<OclVppCLImage>& dst)
{
    uint32_t width = src->getWidth();
    uint32_t height = src->getHeight();
    uint32_t size;
    cl_kernel kernel = NULL;
    if (m_transform & VPP_TRANSFORM_FLIP_H) {
        size = width / 4 - 1;
        kernel = getKernel("transform_flip_h");
    } else if (m_transform & VPP_TRANSFORM_FLIP_V) {
        size = height;
        kernel = getKernel("transform_flip_v");
    }
    if (!kernel) {
        ERROR("failed to get cl kernel");
        return YAMI_FAIL;
    }
    if ((CL_SUCCESS != clSetKernelArg(kernel, 0, sizeof(cl_mem), &dst->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 2, sizeof(cl_mem), &src->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 3, sizeof(cl_mem), &src->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 4, sizeof(uint32_t), &size))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }

    size_t globalWorkSize[2], localWorkSize[2];
    localWorkSize[0] = 8;
    localWorkSize[1] = 8;
    globalWorkSize[0] = ALIGN_POW2(width, localWorkSize[0] * 4) / 4;
    globalWorkSize[1] = ALIGN_POW2(height, localWorkSize[1] * 2) / 2;
    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, kernel, 2, NULL,
        globalWorkSize, localWorkSize, 0, NULL, NULL), "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }
    return YAMI_SUCCESS;
}

YamiStatus OclPostProcessTransform::rotate(const SharedPtr<OclVppCLImage>& src,
                                           const SharedPtr<OclVppCLImage>& dst)
{
    uint32_t width = src->getWidth();
    uint32_t height = src->getHeight();

    uint32_t size, w, h;
    cl_kernel kernel = NULL;
    if (m_transform & VPP_TRANSFORM_ROT_90) {
        size = 4;
        w = width / 4 - 1;
        h = height / 4 - 1;
        kernel = getKernel("transform_rot_90");
    } else if (m_transform & VPP_TRANSFORM_ROT_180) {
        size = 2;
        w = width / 4 - 1;
        h = height;
        kernel = getKernel("transform_rot_180");
    } else if (m_transform & VPP_TRANSFORM_ROT_270) {
        size = 4;
        w = width / 4 - 1;
        h = height / 4 - 1;
        kernel = getKernel("transform_rot_270");
    }
    if (!kernel) {
        ERROR("failed to get cl kernel");
        return YAMI_FAIL;
    }
    if ((CL_SUCCESS != clSetKernelArg(kernel, 0, sizeof(cl_mem), &dst->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 2, sizeof(cl_mem), &src->plane(0))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 3, sizeof(cl_mem), &src->plane(1))) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 4, sizeof(uint32_t), &w)) ||
        (CL_SUCCESS != clSetKernelArg(kernel, 5, sizeof(uint32_t), &h))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }

    size_t globalWorkSize[2], localWorkSize[2];
    localWorkSize[0] = 8;
    localWorkSize[1] = 8;
    globalWorkSize[0] = ALIGN_POW2(width, localWorkSize[0] * 4) / 4;
    globalWorkSize[1] = ALIGN_POW2(height, localWorkSize[1] * size) / size;
    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, kernel, 2, NULL,
        globalWorkSize, localWorkSize, 0, NULL, NULL), "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }
    return YAMI_SUCCESS;
}

const bool OclPostProcessTransform::s_registered =
    VaapiPostProcessFactory::register_<OclPostProcessTransform>(YAMI_VPP_OCL_TRANSFORM);

}
