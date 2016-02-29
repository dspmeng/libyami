/*
 *  oclpostprocess_mosaic.cpp - opencl based mosaic filter
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

#include "oclpostprocess_mosaic.h"
#include "vaapipostprocess_factory.h"
#include "common/common_def.h"
#include "common/log.h"
#include "ocl/oclcontext.h"
#include "vpp/oclvppimage.h"

namespace YamiMediaCodec {

YamiStatus
OclPostProcessMosaic::process(const SharedPtr<VideoFrame>& src,
    const SharedPtr<VideoFrame>& dst)
{
    YamiStatus status = ensureContext("mosaic");
    if (status != YAMI_SUCCESS)
        return status;

    if (dst->fourcc != YAMI_FOURCC_NV12) {
        ERROR("only support mosaic filter on NV12 video frame");
        return YAMI_INVALID_PARAM;
    }

    cl_image_format format;
    format.image_channel_order = CL_R;
    format.image_channel_data_type = CL_UNORM_INT8;
    SharedPtr<OclVppCLImage> imagePtr = OclVppCLImage::create(m_display, dst, m_context, format);
    if (!imagePtr->numPlanes()) {
        ERROR("failed to create cl image from dst frame");
        return YAMI_FAIL;
    }

    cl_mem bgImageMem[3];
    for (uint32_t n = 0; n < imagePtr->numPlanes(); n++) {
        bgImageMem[n] = imagePtr->plane(n);
    }

    size_t globalWorkSize[2], localWorkSize[2];
    localWorkSize[0] = 256 / m_blockSize * m_blockSize;
    localWorkSize[1] = 1;
    globalWorkSize[0] = (dst->crop.width / localWorkSize[0] + 1) * localWorkSize[0];
    globalWorkSize[1] = dst->crop.height / m_blockSize + 1;
    size_t localMemSize = localWorkSize[0] * sizeof(float);

    cl_kernel kernel = getKernel("mosaic");
    if (!kernel) {
        ERROR("failed to get cl kernel");
        return YAMI_FAIL;
    }
    if ((CL_SUCCESS != clSetKernelArg(kernel, 0, sizeof(cl_mem), &imagePtr->plane(0)))
         || (CL_SUCCESS != clSetKernelArg(kernel, 1, sizeof(cl_mem), &bgImageMem[0]))
         || (CL_SUCCESS != clSetKernelArg(kernel, 2, sizeof(cl_mem), &imagePtr->plane(1)))
         || (CL_SUCCESS != clSetKernelArg(kernel, 3, sizeof(cl_mem), &bgImageMem[1]))
         || (CL_SUCCESS != clSetKernelArg(kernel, 4, sizeof(uint32_t), &dst->crop.x))
         || (CL_SUCCESS != clSetKernelArg(kernel, 5, sizeof(uint32_t), &dst->crop.y))
         || (CL_SUCCESS != clSetKernelArg(kernel, 6, sizeof(uint32_t), &m_blockSize))
         || (CL_SUCCESS != clSetKernelArg(kernel, 7, localMemSize, NULL))) {
        ERROR("clSetKernelArg failed");
        return YAMI_FAIL;
    }

    if (!checkOclStatus(clEnqueueNDRangeKernel(m_context->m_queue, kernel, 2, NULL,
                            globalWorkSize, localWorkSize, 0, NULL, NULL),
            "EnqueueNDRangeKernel")) {
        return YAMI_FAIL;
    }

    return status;
}

YamiStatus OclPostProcessMosaic::setParameters(VppParamType type, void* vppParam)
{
    YamiStatus status = YAMI_INVALID_PARAM;

    switch (type) {
    case VppParamTypeMosaic: {
        VppParamMosaic* mosaic = (VppParamMosaic*)vppParam;
        if (mosaic->size == sizeof(VppParamMosaic)) {
            m_blockSize = mosaic->blockSize;
            status = YAMI_SUCCESS;
        }
    } break;
    default:
        status = OclPostProcessBase::setParameters(type, vppParam);
        break;
    }
    return status;
}

const bool OclPostProcessMosaic::s_registered = VaapiPostProcessFactory::register_<OclPostProcessMosaic>(YAMI_VPP_OCL_MOSAIC);
}
