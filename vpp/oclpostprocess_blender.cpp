/*
 *  oclpostprocess_blend.cpp - ocl based alpha blending
 *
 *  Copyright (C) 2015 Intel Corporation
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

#include "oclpostprocess_blender.h"
#include "vaapipostprocess_factory.h"
#include "common/common_def.h"
#include "common/log.h"
#include "ocl/oclcontext.h"
#include <va/va_drmcommon.h>

namespace YamiMediaCodec{

YamiStatus
OclPostProcessBlender::blend(const SharedPtr<VideoFrame>& src,
                             const SharedPtr<VideoFrame>& dst)
{
    YamiStatus ret = YAMI_SUCCESS;
    cl_int cl_status;
    cl_context context = m_context.get()->m_context;
    cl_command_queue cmd_q = m_context.get()->m_queue;
    cl_mem src_cl_mem = NULL;
    cl_mem dst_cl_mem_y = NULL, dst_cl_mem_uv = NULL;
    cl_mem bg_cl_mem_y = NULL, bg_cl_mem_uv = NULL;
    cl_import_image_info_intel import_info;
    size_t global_work_size[2], local_work_size[2];
    uint32_t crop_x, crop_y, crop_w, crop_h;
    uint32_t element_size;

    VASurfaceID src_id = (VASurfaceID)src->surface;
    VASurfaceID dst_id = (VASurfaceID)dst->surface;
    VAImage src_image, dst_image;
    VABufferInfo src_info, dst_info;

    if (VA_STATUS_SUCCESS != vaDeriveImage(m_display, src_id, &src_image)) {
        ERROR("vaDeriveImage of src failed");
        ret = YAMI_DRIVER_FAIL;
        goto err_va;
    }
    src_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    if (VA_STATUS_SUCCESS != vaAcquireBufferHandle(m_display, src_image.buf, &src_info)) {
        ERROR("vaAcquireBufferHandle of src failed");
        ret = YAMI_DRIVER_FAIL;
        goto err_va;
    }
    if (VA_STATUS_SUCCESS != vaDeriveImage(m_display, dst_id, &dst_image)) {
        ERROR("vaDeriveImage of dst failed");
        ret = YAMI_DRIVER_FAIL;
        goto err_va;
    }
    dst_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    if (VA_STATUS_SUCCESS != vaAcquireBufferHandle(m_display, dst_image.buf, &dst_info)) {
        ERROR("vaAcquireBufferHandle of src failed");
        ret = YAMI_DRIVER_FAIL;
        goto err_va;
    }

    if (src_image.format.fourcc != VA_FOURCC_RGBA ||
        dst_image.format.fourcc != VA_FOURCC_NV12) {
        ERROR("only support RGBA blending on NV12");
        ret = YAMI_INVALID_PARAM;
        goto err_va;
    }

    import_info.fd = src_info.handle;
    import_info.type = CL_MEM_OBJECT_IMAGE2D;
    import_info.fmt.image_channel_order = CL_RGBA;
    import_info.fmt.image_channel_data_type = CL_UNORM_INT8;
    import_info.row_pitch = src_image.pitches[0];
    import_info.offset = src_image.offsets[0];
    import_info.width = src.get()->crop.width;
    import_info.height = src.get()->crop.height;
    import_info.size = import_info.row_pitch * import_info.height;
    src_cl_mem = clCreateImageFromFdINTEL(context, &import_info, &cl_status);
    if (cl_status != CL_SUCCESS) {
        ERROR("clCreateImageFromFdINTEL failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }

    element_size = 2;
    import_info.fd = dst_info.handle;
    import_info.type = CL_MEM_OBJECT_IMAGE2D;
    import_info.fmt.image_channel_order = CL_RG;
    import_info.fmt.image_channel_data_type = CL_UNORM_INT8;
    import_info.row_pitch = dst_image.pitches[0];
    import_info.offset = dst_image.offsets[0];
    import_info.width =  dst_image.width / element_size;
    import_info.height = dst_image.height;
    import_info.size = import_info.row_pitch * import_info.height;
    dst_cl_mem_y = clCreateImageFromFdINTEL(context, &import_info, &cl_status);
    if (cl_status != CL_SUCCESS) {
        ERROR("clCreateImageFromFdINTEL failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }
    bg_cl_mem_y = clCreateImageFromFdINTEL(context, &import_info, &cl_status);
    if (cl_status != CL_SUCCESS) {
        ERROR("clCreateImageFromFdINTEL failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }

    import_info.fd = dst_info.handle;
    import_info.type = CL_MEM_OBJECT_IMAGE2D;
    import_info.fmt.image_channel_order = CL_RG;
    import_info.fmt.image_channel_data_type = CL_UNORM_INT8;
    import_info.row_pitch = dst_image.pitches[1];
    import_info.offset = dst_image.offsets[1];
    import_info.width = dst_image.width / element_size;
    import_info.height = dst_image.height / 2;
    import_info.size = import_info.row_pitch * import_info.height / 2;
    dst_cl_mem_uv = clCreateImageFromFdINTEL(context, &import_info, &cl_status);
    if (cl_status != CL_SUCCESS) {
        ERROR("clCreateImageFromFdINTEL failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }
    bg_cl_mem_uv = clCreateImageFromFdINTEL(context, &import_info, &cl_status);
    if (cl_status != CL_SUCCESS) {
        ERROR("clCreateImageFromFdINTEL failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }

    crop_x = dst.get()->crop.x / element_size;
    crop_y = dst.get()->crop.y & ~1;
    crop_w = dst.get()->crop.width / element_size;
    crop_h = dst.get()->crop.height;
    if ((cl_status = clSetKernelArg(m_kernel, 0, sizeof(cl_mem), &dst_cl_mem_y))  ||
        (cl_status = clSetKernelArg(m_kernel, 1, sizeof(cl_mem), &dst_cl_mem_uv)) ||
        (cl_status = clSetKernelArg(m_kernel, 2, sizeof(cl_mem), &bg_cl_mem_y))   ||
        (cl_status = clSetKernelArg(m_kernel, 3, sizeof(cl_mem), &bg_cl_mem_uv))  ||
        (cl_status = clSetKernelArg(m_kernel, 4, sizeof(cl_mem), &src_cl_mem))    ||
        (cl_status = clSetKernelArg(m_kernel, 5, sizeof(uint32_t), &crop_x))   ||
        (cl_status = clSetKernelArg(m_kernel, 6, sizeof(uint32_t), &crop_y))   ||
        (cl_status = clSetKernelArg(m_kernel, 7, sizeof(uint32_t), &crop_w)) ||
        (cl_status = clSetKernelArg(m_kernel, 8, sizeof(uint32_t), &crop_h))) {
        ERROR("clSetKernelArg failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }

    // each work group has 8x8 work items; each work item handles 2x2 pixels
    local_work_size[0] = 8;
    local_work_size[1] = 8;
    global_work_size[0] = ALIGN16(dst.get()->crop.width) / element_size;
    global_work_size[1] = ALIGN16(dst.get()->crop.height) / 2;
    cl_status = clEnqueueNDRangeKernel(cmd_q, m_kernel, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL);
    if (cl_status != CL_SUCCESS) {
        printf("clEnqueueNDRangeKernel failed with %d\n", cl_status);
        ret = YAMI_FAIL;
        goto err_cl;
    }
    clFlush(cmd_q);
    clFinish(cmd_q);

err_cl:
    if (src_cl_mem) clReleaseMemObject(src_cl_mem);
    if (dst_cl_mem_y) clReleaseMemObject(dst_cl_mem_y);
    if (dst_cl_mem_uv) clReleaseMemObject(dst_cl_mem_uv);
    if (bg_cl_mem_y) clReleaseMemObject(bg_cl_mem_y);
    if (bg_cl_mem_uv) clReleaseMemObject(bg_cl_mem_uv);
err_va:
    vaReleaseBufferHandle(m_display, src_image.buf);
    vaDestroyImage(m_display, src_image.image_id);
    vaReleaseBufferHandle(m_display, dst_image.buf);
    vaDestroyImage(m_display, dst_image.image_id);

    return ret;
}

YamiStatus
OclPostProcessBlender::process(const SharedPtr<VideoFrame>& src,
                               const SharedPtr<VideoFrame>& dest)
{
    YamiStatus status = ensureContext("blend");
    if (status != YAMI_SUCCESS)
        return status;

    blend(src, dest);

    return YAMI_SUCCESS;
}

const bool OclPostProcessBlender::s_registered =
    VaapiPostProcessFactory::register_<OclPostProcessBlender>(YAMI_VPP_OCL_BLENDER);

}

