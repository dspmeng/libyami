/*
 *  vaapidecsurfacepool.cpp - surface pool for decoder
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "vaapidecsurfacepool.h"

#include "common/log.h"
#include "vaapi/vaapidisplay.h"
#include "vaapi/vaapisurface.h"
#include "vaapi/vaapiimagepool.h"
#include <string.h>
#include <assert.h>

namespace YamiMediaCodec{
const uint32_t IMAGE_POOL_SIZE = 8;

DecSurfacePoolPtr VaapiDecSurfacePool::create(const DisplayPtr& display, VideoConfigBuffer* config)
{
    DecSurfacePoolPtr pool;
    std::vector<SurfacePtr> surfaces;
    size_t size = config->surfaceNumber;
    surfaces.reserve(size);
    assert(!(config->flag & WANT_SURFACE_PROTECTION));
    assert(!(config->flag & WANT_RAW_OUTPUT));
    for (size_t i = 0; i < size; ++i) {
        SurfacePtr s = VaapiSurface::create(display, VAAPI_CHROMA_TYPE_YUV420,
                                   config->surfaceWidth,config->surfaceHeight,NULL,0);
        if (!s)
            return pool;
        s->resize(config->width, config->height);
        surfaces.push_back(s);
    }
    DecSurfacePoolPtr temp(new VaapiDecSurfacePool(display, surfaces));
    pool = temp;
    return pool;
}

VaapiDecSurfacePool::VaapiDecSurfacePool(const DisplayPtr& display, std::vector<SurfacePtr> surfaces):
    m_display(display),
    m_cond(m_lock),
    m_flushing(false)
{
    size_t size = surfaces.size();
    m_surfaces.swap(surfaces);
    m_renderBuffers.resize(size);
    for (size_t i = 0; i < size; ++i) {
        const SurfacePtr& s = m_surfaces[i];
        VASurfaceID id = m_surfaces[i]->getID();
        m_renderBuffers[i].display = display->getID();
        m_renderBuffers[i].surface = id;
        m_renderBuffers[i].timeStamp = 0;

        m_renderMap[id] = &m_renderBuffers[i];
        m_surfaceMap[id] = s.get();
        m_freed.push_back(id);
    }
}

void VaapiDecSurfacePool::getSurfaceIDs(std::vector<VASurfaceID>& ids)
{
    //no need hold lock, it never changed from start
    assert(!ids.size());
    size_t size = m_renderBuffers.size();
    ids.reserve(size);

    for (size_t i = 0; i < size; ++i)
        ids.push_back(m_renderBuffers[i].surface);
}

struct VaapiDecSurfacePool::SurfaceRecycler
{
    SurfaceRecycler(const DecSurfacePoolPtr& pool): m_pool(pool) {}
    void operator()(VaapiSurface* surface) { m_pool->recycle(surface->getID(), SURFACE_DECODING);}
private:
    DecSurfacePoolPtr m_pool;
};

SurfacePtr VaapiDecSurfacePool::acquireWithWait()
{
    SurfacePtr surface;
    AutoLock lock(m_lock);
    while (m_freed.empty() && !m_flushing) {
        DEBUG("wait because there is no available surface from pool");
        m_cond.wait();
    }

    if (m_flushing) {
        DEBUG("input flushing, return nil surface");
        return surface;
    }

    assert(!m_freed.empty());
    VASurfaceID id = m_freed.front();
    m_freed.pop_front();
    m_allocated[id] = SURFACE_DECODING;
    VaapiSurface* s = m_surfaceMap[id];
    surface.reset(s, SurfaceRecycler(shared_from_this()));
    return surface;
}

bool VaapiDecSurfacePool::output(const SurfacePtr& surface, int64_t timeStamp)
{
    VASurfaceID id = surface->getID();

    AutoLock lock(m_lock);
    const Allocated::iterator it = m_allocated.find(id);
    if (it == m_allocated.end())
        return false;
    assert(it->second == SURFACE_DECODING);
    it->second |= SURFACE_TO_RENDER;
    VideoRenderBuffer* buffer = m_renderMap[id];
    buffer->timeStamp = timeStamp;
    DEBUG("surface=0x%x is output-able with timeStamp=%ld", surface->getID(), timeStamp);
    m_output.push_back(buffer);
    return true;
}

VideoRenderBuffer* VaapiDecSurfacePool::getOutput()
{
    AutoLock lock(m_lock);
    if (m_output.empty())
        return NULL;
    VideoRenderBuffer* buffer = m_output.front();
    m_output.pop_front();
    const Allocated::iterator it = m_allocated.find(buffer->surface);
    assert(it != m_allocated.end());
    assert(it->second & SURFACE_TO_RENDER);
    assert(!(it->second & SURFACE_RENDERING));
    //clear SURFACE_TO_RENDER and set SURFACE_RENDERING
    it->second ^= SURFACE_RENDERING | SURFACE_TO_RENDER;
    return buffer;
}

struct VaapiDecSurfacePool::SurfaceRecyclerRender
{
    SurfaceRecyclerRender(const DecSurfacePoolPtr& pool, VideoRenderBuffer* buffer): m_pool(pool), m_buffer(buffer) {}
    void operator()(VaapiSurface* surface) { m_pool->recycle(m_buffer);}
private:
    DecSurfacePoolPtr m_pool;
    VideoRenderBuffer* m_buffer;
};

void VaapiDecSurfacePool::setWaitable(bool waitable)
{
    m_flushing = !waitable;

    if (!waitable) {
        m_cond.signal();
    }
}

void VaapiDecSurfacePool::flush()
{
    AutoLock lock(m_lock);
    for (OutputQueue::iterator it = m_output.begin();
        it != m_output.end(); ++it) {
        recycleLocked((*it)->surface, SURFACE_TO_RENDER);
    }
    m_output.clear();
    //still have unreleased surface
    if (!m_allocated.empty())
        m_flushing = true;
}

void VaapiDecSurfacePool::recycleLocked(VASurfaceID id, SurfaceState flag)
{
    const Allocated::iterator it = m_allocated.find(id);
    if (it == m_allocated.end()) {
        ERROR("try to recycle %u from state %d, it's not an allocated buffer", id, flag);
        return;
    }
    it->second &= ~flag;
    if (it->second == SURFACE_FREE) {
        m_allocated.erase(it);
        m_freed.push_back(id);
        if (m_flushing && m_allocated.size() == 0)
            m_flushing = false;
        m_cond.signal();
    }
}

void VaapiDecSurfacePool::recycle(VASurfaceID id, SurfaceState flag)
{
    AutoLock lock(m_lock);
    recycleLocked(id,flag);
}

void VaapiDecSurfacePool::recycle(const VideoRenderBuffer * renderBuf)
{
    if (renderBuf < &m_renderBuffers[0]
        || renderBuf >= &m_renderBuffers[m_renderBuffers.size()]) {
        ERROR("recycle invalid render buffer");
        return;
    }
    recycle(renderBuf->surface, SURFACE_RENDERING);
}

} //namespace YamiMediaCodec
