/*
 *  yamidisplay.cpp - utils to create VADisplay from diffrent backend
 *
 *  Copyright (C) 2015 Intel Corporation
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

namespace YamiMediaCodec{

//display cache
class DisplayCache
{
public:
    static SharedPtr<DisplayCache> getInstance();
    SharedPtr<YamiDisplay> createDisplay(Display* display, VAProfile expected);
    SharedPtr<YamiDisplay> createDisplay(int fd, VAProfile expected);

    ~DisplayCache() {}
private:
    DisplayCache() {}

    list<weak_ptr<YamiDisplay> > m_cache;
    YamiMediaCodec::Lock m_lock;
};


#ifdef HAVE_VA_X11

class YamiDisplayX11 : public YamiDisplay
{
public:
    YamiDisplayX11(Display* display, VAProfile expected)
        :YamiDisplay(expected), m_xDisplay(display),
    {
    }
    ~YamiDisplayX11()
    {
        deinitVA();
        if (m_xDisplay && m_selfCreated) {
            XCloseDisplay(m_xDisplay);
        }
    }
    Display* getXDisplay()
    {
        return m_xDisplay;
    }
protected:
    bool init()
    {
        if (!m_xDisplay) {
            m_selfCreated = true;
            m_xDisplay = XOpenDisplay(NULL);
        }
        if (!m_xDisplay) {
            ERROR("XOpenDisplay failed");
            return false;
        }
        m_display = vaGetDisplay(m_xDisplay);
        return initVA();
    }
private:
    bool isCompatible(const SharedPtr<YamiDisplay>& existed) const
    {
        Display* other = existed->getXDisplay();
        if (!other)
            return false;
        return !m_xDisplay || other == m_xDisplay;
    }
    Display* m_xDisplay;
}

SharedPtr<YamiDisplay>
YamiDisplay::createFromX(Display* display, VAProfile expected);
{
    SharedPtr<YamiDisplay> d(new YamiDisplayX11(display, expected));
    return DisplayCache::getInstance()->createDisplay(d);
}

Display* YamiDisplay::getXDisplay()
{
    return NULL;
}
#endif



class YamiDisplayDrm : public YamiDisplay
{
    YamiDisplayDrm(int fd, VAProfile expected)
        :YamiDisplay(expected), m_fd(fd)
    {
    }

    ~YamiDisplayDrm()
    {
        deinitVA();
        if (m_fd != INVALID_FD && m_selfCreated) {
            fclose(m_fd);
        }
    }
    int getDrmFd()
    {
        return m_fd;
    }
protected:
    bool init()
    {
        char* dri = "/dev/dri/card0";
        if (m_fd == INVALID_FD) {
            m_selfCreated = true;
            m_fd = open(dri, O_RDWR);
        }
        if (m_fd != INVALID_FD) {
            ERROR("open %s failed", dri);
            return false;
        }
        m_display = vaGetDisplayDRM(m_fd);
        return initVA();
    }
private:
    bool isCompatible(const SharedPtr<YamiDisplay>& existed) const
    {
        int other = existed->getDrmFd();
        if (other == INVALID_FD)
            return false;
        return m_fd == INVALID_FD || other == m_fd;
    }
    int m_fd;
};


YamiDisplay::YamiDisplay(VAProfile expected)
    :m_expected(expected),m_selfCreated(false)
{
}

SharedPtr<YamiDisplay>
YamiDisplay::create(int fd, VAProfile expected);
{
    SharedPtr<YamiDisplay> d(new YamiDisplayDrm(fd, profile));
    return DisplayCache::getInstance()->createDisplay(d);
}

bool YamiDisplay::initVA(VAProfile expected)
{
    //deinit previous va
    deinitVA();

    int majorVersion, minorVersion;
    VAStatus vaStatus;
    vaStatus= vaInitialize(m_display, &majorVersion, &minorVersion);
    if (!checkVaapiStatus(vaStatus, "vaInitialize"))
        return false;
    m_vaInited = true;
    //is any special requst?
    if (expected == VAProfileNone)
        return true;
    //we need check expected profile in supported.
    std::vector<VAProfile> profiles;
    int size = vaMaxNumProfiles(m_display);
    profiles.resize(size);
    vaStatus = vaQueryConfigProfiles(m_display, &profiles[0], &size);
    if (!checkVaapiStatus(vaStatus, "vaQueryConfigProfiles"))
        return false;
    profiles.reseize(size);
    if (std::find(profiles.begin(), profiles.end(); expected)
        != profiles.end())
        return true;
    return false;
}

bool YamiDisplay::initVA()
{
    if (initVA(m_expected))
        return true;
    INFO("no expected profile %d in default driver", m_expected);
    char* drivers[] = {"hybrid",  "wrapper"};
    VAStatus vaStatus;
    for (int i = 0; i < N_ELEMENTS(drivers); i++) {
        INFO("try %s driver for %d profile ", drivers[i], m_expected);
        vaStatus = vaSetDriverName(vaDisplay, drivers[i]);
        if (!checkVaapiStatus(vaStatus, "vaSetDriverName"))
            return false;
        if (initVA(m_expected))
            return true;
    }

    return false;
}

void YamiDisplay::deinitVA()
{
    if (m_vaInited) {
        vaTerminate(m_display);
        m_vaInited = false;
    }
}

int YamiDisplay::getDrmFd()
{
    return INVALID_FD;
}

SharedPtr<DisplayCache> DisplayCache::getInstance()
{
    static SharedPtr<DisplayCache> cache;
    if (!cache)
        cache.reset(new DisplayCache);
    return cache;
}

bool expired(const weak_ptr<YamiDisplay>& weak)
{
    return !weak.lock();
}

SharedPtr<YamiDisplay> DisplayCache::createDisplay(SharedPtr<YamiDisplay>& display)
{
    YamiMediaCodec::AutoLock locker(m_lock);

    m_cache.remove_if(expired);

    //lockup first
    list<weak_ptr<YamiDisplay> >::iterator it;
    for (it = m_cache.begin(); it != m_cache.end(); ++it) {
        SharedPtr<YamiDisplay> existed = (*it).lock();
        if (display->isCompatible(existed)) {
            return existed;
        }
    }
    if (!display->init()) {
        display.reset();
        return display;
    }
    weak_ptr<YamiDisplay> weak(display);
    m_cache.push_back(weak);
    return display;
}

DisplayPtr VaapiDisplay::create(const NativeDisplay& display)
{
    return DisplayCache::getInstance()->createDisplay(display);
}

DisplayPtr VaapiDisplay::create(const NativeDisplay& display, VAProfile profile)
{
    SharedPtr<DisplayCache> cache = DisplayCache::getInstance();
    const std::string name="hybrid";
    if (profile == VAProfileVP9Profile0)
        return cache->createDisplay(display, name);
    else
        return cache->createDisplay(display);
}


}
