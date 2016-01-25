/*
 *  oclvppimage.h - image wrappers for opencl vpp modules
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

#ifndef oclvppimage_h
#define oclvppimage_h

#include "VideoCommonDefs.h"
#include <CL/opencl.h>
#include <va/va.h>

namespace YamiMediaCodec{
/**
 * \class OclVppImage
 * \brief Image wrappers used by opencl vpp modules
 *
 */
template <typename MemType>
class OclVppImage {
public:
    OclVppImage(const SharedPtr<VideoFrame>& f, VADisplay d)
        : m_frame(f), m_display(d)
    {
        memset(&m_image, 0, sizeof(m_image));
        m_image.image_id = VA_INVALID_ID;
        m_image.buf = VA_INVALID_ID;
    }
    virtual ~OclVppImage() {}
    const MemType& plane(int n) {return m_mem[n];}
    uint32_t pitch(int n) {return m_image.pitches[n];}
    uint32_t numPlanes() {return m_image.num_planes;}

protected:
    SharedPtr<VideoFrame> m_frame;
    VADisplay m_display;
    VAImage   m_image;
    MemType   m_mem[3];
};

class OclVppRawImage: public OclVppImage<uint8_t*> {
public:
    OclVppRawImage(const SharedPtr<VideoFrame>& f, VADisplay d);
    ~OclVppRawImage();
    uint8_t pixel(uint32_t x, uint32_t y, int n) {return m_mem[n][y * pitch(n) + x];}
};

class OclVppCLImage: public OclVppImage<cl_mem> {
public:
    OclVppCLImage(const SharedPtr<VideoFrame>& f,
                  VADisplay d,
                  const SharedPtr<OclContext> ctx,
                  const cl_image_format& fmt);
    ~OclVppCLImage();
};
}
#endif                          /* oclvppimage_h */
