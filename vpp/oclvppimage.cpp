/*
 *  oclvppimage.cpp - image wrappers for opencl vpp modules
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

#include "common/log.h"
#include "ocl/oclcontext.h"
#include "oclvppimage.h"
#include "vaapi/vaapiutils.h"
#include <va/va_drmcommon.h>

namespace YamiMediaCodec{

OclVppRawImage::OclVppRawImage(
    const SharedPtr<VideoFrame>& f,
    VADisplay d)
    : OclVppImage<uint8_t *>(f, d)
{
    VASurfaceID surfaceId = (VASurfaceID)m_frame->surface;
    if (!checkVaapiStatus(vaDeriveImage(m_display, surfaceId, &m_image), "DeriveImage"))
        return;

    uint8_t* buf = 0;
    if (!checkVaapiStatus(vaMapBuffer(m_display, m_image.buf, (void**)&buf), "vaMapBuffer"))
        return;

    for (uint32_t n = 0; n < m_image.num_planes; n++) {
        m_mem[n] = buf + m_image.offsets[n];
    }
}

OclVppRawImage::~OclVppRawImage()
{
    checkVaapiStatus(vaUnmapBuffer(m_display, m_image.buf), "ReleaseBufferHandle");
    checkVaapiStatus(vaDestroyImage(m_display, m_image.image_id), "DestroyImage");
}

OclVppCLImage::OclVppCLImage(
    const SharedPtr<VideoFrame>& f,
    VADisplay d,
    const SharedPtr<OclContext> ctx,
    const cl_image_format& fmt)
    : OclVppImage<cl_mem>(f, d)
{
    VASurfaceID surfaceId = (VASurfaceID)m_frame->surface;
    if (!checkVaapiStatus(vaDeriveImage(m_display, surfaceId, &m_image), "DeriveImage"))
        return;

    VABufferInfo bufferInfo;
    bufferInfo.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    if (!checkVaapiStatus(vaAcquireBufferHandle(m_display, m_image.buf, &bufferInfo),
        "AcquireBufferHandle"))
        return;

    cl_import_image_info_intel importInfo;
    uint32_t height[3];
    switch (m_image.format.fourcc) {
    case VA_FOURCC_RGBA:
        height[0] = m_image.height;
        break;
    case VA_FOURCC_NV12:
        height[0] = m_image.height;
        height[1] = m_image.height / 2;
        break;
    default:
        ERROR("unsupported format");
        return;
    }

    for (uint32_t n = 0; n < m_image.num_planes; n++) {
        importInfo.fd = bufferInfo.handle;
        importInfo.type = CL_MEM_OBJECT_IMAGE2D;
        importInfo.fmt.image_channel_order = fmt.image_channel_order;
        importInfo.fmt.image_channel_data_type = fmt.image_channel_data_type;
        importInfo.row_pitch = m_image.pitches[n];
        importInfo.offset = m_image.offsets[n];
        importInfo.width = m_image.width;
        importInfo.height = height[n];
        importInfo.size = importInfo.row_pitch * importInfo.height;
        if (YAMI_SUCCESS != ctx->createImageFromFdIntel(&importInfo, &m_mem[n])) {
            ERROR("createImageFromFdIntel failed");
            break;
        }
    }

    return;
}

OclVppCLImage::~OclVppCLImage()
{
    for (uint32_t n = 0; n < m_image.num_planes; n++)
        checkOclStatus(clReleaseMemObject(m_mem[n]), "ReleaseMemObject");

    checkVaapiStatus(vaReleaseBufferHandle(m_display, m_image.buf), "ReleaseBufferHandle");
    checkVaapiStatus(vaDestroyImage(m_display, m_image.image_id), "DestroyImage");
}
}
