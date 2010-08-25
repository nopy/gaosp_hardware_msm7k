/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QComHardwareRenderer.h"

#include <binder/MemoryHeapBase.h>
#include <binder/MemoryHeapPmem.h>
#include <media/stagefright/MediaDebug.h>
#include <surfaceflinger/ISurface.h>

#include <cutils/properties.h>
#include <sys/time.h>

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "QComHardwareRenderer"
#include <utils/Log.h>

namespace android {

////////////////////////////////////////////////////////////////////////////////

typedef struct PLATFORM_PRIVATE_ENTRY
{
    /* Entry type */
    uint32_t type;

    /* Pointer to platform specific entry */
    void *entry;

} PLATFORM_PRIVATE_ENTRY;

typedef struct PLATFORM_PRIVATE_LIST
{
    /* Number of entries */
    uint32_t nEntries;

    /* Pointer to array of platform specific entries *
     * Contiguous block of PLATFORM_PRIVATE_ENTRY elements */
    PLATFORM_PRIVATE_ENTRY *entryList;

} PLATFORM_PRIVATE_LIST;

// data structures for tunneling buffers
typedef struct PLATFORM_PRIVATE_PMEM_INFO
{
    /* pmem file descriptor */
    uint32_t pmem_fd;
    uint32_t offset;

} PLATFORM_PRIVATE_PMEM_INFO;

#define PLATFORM_PRIVATE_PMEM   1

QComHardwareRenderer::QComHardwareRenderer(
        const sp<ISurface> &surface,
        size_t displayWidth, size_t displayHeight,
        size_t decodedWidth, size_t decodedHeight)
    : mISurface(surface),
      mDisplayWidth(displayWidth),
      mDisplayHeight(displayHeight),
      mDecodedWidth(decodedWidth),
      mDecodedHeight(decodedHeight),
      mFrameSize((mDecodedWidth * mDecodedHeight * 3) / 2),
      mStatistics(false),
      mLastFrame(0),
      mFpsSum(0),
      mFrameNumber(0),
      mNumFpsSamples(0),
      mLastFrameTime(0) {
    CHECK(mISurface.get() != NULL);
    CHECK(mDecodedWidth > 0);
    CHECK(mDecodedHeight > 0);

    char value[PROPERTY_VALUE_MAX];
    property_get("persist.debug.sf.statistics",value,"0");
    if (atoi(value)) mStatistics = true;
}

QComHardwareRenderer::~QComHardwareRenderer() {
    if (mStatistics) AverageFPSPrint();

    mISurface->unregisterBuffers();
}

void QComHardwareRenderer::render(
        const void *data, size_t size, void *platformPrivate) {
    size_t offset;
    if (!getOffset(platformPrivate, &offset)) {
        return;
    }

    mISurface->postBuffer(offset);

    //Average FPS Profiling
    if (mStatistics) AverageFPSProfiling();
}

bool QComHardwareRenderer::getOffset(void *platformPrivate, size_t *offset) {
    *offset = 0;

    PLATFORM_PRIVATE_LIST *list = (PLATFORM_PRIVATE_LIST *)platformPrivate;
    for (uint32_t i = 0; i < list->nEntries; ++i) {
        if (list->entryList[i].type != PLATFORM_PRIVATE_PMEM) {
            continue;
        }

        PLATFORM_PRIVATE_PMEM_INFO *info =
            (PLATFORM_PRIVATE_PMEM_INFO *)list->entryList[i].entry;

        if (info != NULL) {
            if (mMemoryHeap.get() == NULL) {
                publishBuffers(info->pmem_fd);
            }

            if (mMemoryHeap.get() == NULL) {
                return false;
            }

            *offset = info->offset;

            return true;
        }
    }

    return false;
}

void QComHardwareRenderer::publishBuffers(uint32_t pmem_fd) {
    sp<MemoryHeapBase> master =
        reinterpret_cast<MemoryHeapBase *>(pmem_fd);

    master->setDevice("/dev/pmem");

    uint32_t heap_flags = master->getFlags() & MemoryHeapBase::NO_CACHING;
    mMemoryHeap = new MemoryHeapPmem(master, heap_flags);
    mMemoryHeap->slap();

    ISurface::BufferHeap bufferHeap(
            mDisplayWidth, mDisplayHeight,
            mDecodedWidth, mDecodedHeight,
            HAL_PIXEL_FORMAT_YCrCb_420_SP,
            mMemoryHeap);

    status_t err = mISurface->registerBuffers(bufferHeap);
    CHECK_EQ(err, OK);
}

void QComHardwareRenderer::AverageFPSPrint() {
    LOGW("=========================================================");
    LOGW("Average Frames Per Second: %.4f", mFpsSum/mNumFpsSamples);
    LOGW("=========================================================");
}

void QComHardwareRenderer::AverageFPSProfiling() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int64_t now = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    int64_t diff = now - mLastFrameTime;
    mFrameNumber++;

    if (diff > 250000) {
        float mFps = ((mFrameNumber - mLastFrame) * 1E6)/diff;
        LOGW("Frames Per Second: %.4f",mFps);
        mFpsSum += mFps;
        mNumFpsSamples++;
        mLastFrameTime = now;
        mLastFrame = mFrameNumber;
    }
}

}  // namespace android
