/*
 *  yamidisplay.h - utils to creat VADisplay from diffrent backend
 *
 *  Copyright (C) 2014 Intel Corporation
 *    Author: Xu Guangxin <guangxin.xu@intel.com>
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

#ifndef vaapidisplay_h
#define vaapidisplay_h

#include <va/va.h>
#include <va/va_tpi.h>
#ifdef HAVE_VA_X11
#include <va/va_x11.h>
#endif
#include <va/va_drm.h>

///abstract for all display, x11, wayland, ozone, android etc.
namespace YamiMediaCodec{
#ifndef VA_FOURCC_I420
#define VA_FOURCC_I420 VA_FOURCC('I','4','2','0')
#endif

/**
 * \class YamiDisplay
 * \brief utils to init and destory libva
 *
 * it handle 4 things:
 *  1. select libva driver name base on profile
 *  2. create and destory libva backend, such as x11 display, drm handle
 *  3. manager libva init/terminate life cycle
 *  4. create a cache to share VADisplay in same process. (eg. encoder and decoder)
 */
class YamiDisplay
{
#ifdef HAVE_VA_X11
public:
    /** \brief create YamiDisplay object
     * @param[in] display if it's NULL, we will create a Display for you.
     * we will using vaGetDisplay to get VADisplay from @param display
     * @param[in] expected expected profile
     * we will use this profile to select libva driver.
     */
    static SharedPtr<YamiDisplay>
    createFromX(Display* display, VAProfile expected = VAProfileNone);

    /** \brief get current Display*
     * it's useful when you pass NULL to #createFromX
     */
    virtual Display* getXDisplay() = 0;
#endif

public:
    static const int INVALID_FD = -1;

    /** \brief create YamiDisplay object
     * @param[in] fd if it's #INVALID_FD, we will open a fd for you.
     * we will using vaGetDisplayDRM to get VADisplay from @param fd
     * @param[in] expected expected profile
     * we will use this profile to select libva driver.
     */
    static SharedPtr<YamiDisplay>
    createFromDrm(int fd = INVALID_FD, VAProfile expected = VAProfileNone);

    /** \brief get current drm fd
     * it's useful when you pass #INVALID_FD to #createFromDrm
     */
    virtual int getDrmFd() const;

protected:
    bool init() = 0;

private:
    virtual bool isCompatible(const SharedPtr<YamiDisplay>& existed) const = 0;
    VAProfile m_expected;
    bool m_selfCreated;
    bool m_vaInited;

    DISALLOW_COPY_AND_ASSIGN(YamiDisplay);
};

}
#endif

