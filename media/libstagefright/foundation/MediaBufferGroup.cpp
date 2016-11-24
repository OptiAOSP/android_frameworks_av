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

#define LOG_TAG "MediaBufferGroup"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>

namespace android {

// std::min is not constexpr in C++11
template<typename T>
constexpr T MIN(const T &a, const T &b) { return a <= b ? a : b; }

// MediaBufferGroup may create shared memory buffers at a
// smaller threshold than an isolated new MediaBuffer.
static const size_t kSharedMemoryThreshold = MIN(
        (size_t)MediaBuffer::kSharedMemThreshold, (size_t)(4 * 1024));

MediaBufferGroup::MediaBufferGroup(size_t growthLimit) :
    mGrowthLimit(growthLimit) {
}

MediaBufferGroup::MediaBufferGroup(size_t buffers, size_t buffer_size, size_t growthLimit)
    : mGrowthLimit(growthLimit) {

    if (buffer_size >= kSharedMemoryThreshold) {
        ALOGD("creating MemoryDealer");
        // Using a single MemoryDealer is efficient for a group of shared memory objects.
        // This loop guarantees that we use shared memory (no fallback to malloc).

        size_t alignment = MemoryDealer::getAllocationAlignment();
        size_t augmented_size = buffer_size + sizeof(MediaBuffer::SharedControl);
        size_t total = (augmented_size + alignment - 1) / alignment * alignment * buffers;
        sp<MemoryDealer> memoryDealer = new MemoryDealer(total, "MediaBufferGroup");

        for (size_t i = 0; i < buffers; ++i) {
            sp<IMemory> mem = memoryDealer->allocate(augmented_size);
            if (mem.get() == nullptr) {
                ALOGW("Only allocated %zu shared buffers of size %zu", i, buffer_size);
                break;
            }
            MediaBuffer *buffer = new MediaBuffer(mem);
            buffer->getSharedControl()->clear();
            add_buffer(buffer);
        }
        return;
    }

    // Non-shared memory allocation.
    for (size_t i = 0; i < buffers; ++i) {
        MediaBuffer *buffer = new MediaBuffer(buffer_size);
        if (buffer->data() == nullptr) {
            delete buffer; // don't call release, it's not properly formed
            ALOGW("Only allocated %zu malloc buffers of size %zu", i, buffer_size);
            break;
        }
        add_buffer(buffer);
    }
}

MediaBufferGroup::~MediaBufferGroup() {
    for (MediaBuffer *buffer : mBuffers) {
        buffer->resolvePendingRelease();
        // If we don't release it, perhaps noone will release it.
        LOG_ALWAYS_FATAL_IF(buffer->refcount() != 0,
                "buffer refcount %p = %d != 0", buffer, buffer->refcount());
        // actually delete it.
        buffer->setObserver(nullptr);
        buffer->release();
    }
}

void MediaBufferGroup::add_buffer(MediaBuffer *buffer) {
    Mutex::Autolock autoLock(mLock);

    buffer->setObserver(this);
    mBuffers.emplace_back(buffer);
    // optionally: mGrowthLimit = max(mGrowthLimit, mBuffers.size());
}

void MediaBufferGroup::gc(size_t freeBuffers) {
    Mutex::Autolock autoLock(mLock);

    size_t freeCount = 0;
    for (auto it = mBuffers.begin(); it != mBuffers.end(); ) {
        (*it)->resolvePendingRelease();
        if ((*it)->isDeadObject()) {
            // The MediaBuffer has been deleted, why is it in the MediaBufferGroup?
            LOG_ALWAYS_FATAL("buffer(%p) has dead object with refcount %d",
                    (*it), (*it)->refcount());
        } else if ((*it)->refcount() == 0 && ++freeCount > freeBuffers) {
            (*it)->setObserver(nullptr);
            (*it)->release();
            it = mBuffers.erase(it);
        } else {
            ++it;
        }
    }
}

bool MediaBufferGroup::has_buffers() {
    if (mBuffers.size() < mGrowthLimit) {
        return true; // We can add more buffers internally.
    }
    for (MediaBuffer *buffer : mBuffers) {
        buffer->resolvePendingRelease();
        if (buffer->refcount() == 0) {
            return true;
        }
    }
    return false;
}

#ifdef ADD_LEGACY_ACQUIRE_BUFFER_SYMBOL
extern "C" status_t _ZN7android16MediaBufferGroup14acquire_bufferEPPNS_11MediaBufferE(
    MediaBufferGroup* group, MediaBuffer **out) {
    return group->acquire_buffer(out, false);
}
#endif

status_t MediaBufferGroup::acquire_buffer(
        MediaBuffer **out, bool nonBlocking, size_t requestedSize) {
    Mutex::Autolock autoLock(mLock);
    for (;;) {
        size_t smallest = requestedSize;
        MediaBuffer *buffer = nullptr;
        auto free = mBuffers.end();
        for (auto it = mBuffers.begin(); it != mBuffers.end(); ++it) {
            (*it)->resolvePendingRelease();
            if ((*it)->refcount() == 0) {
                const size_t size = (*it)->size();
                if (size >= requestedSize) {
                    buffer = *it;
                    break;
                }
                if (size < smallest) {
                    smallest = size; // always free the smallest buf
                    free = it;
                }
            }
        }
        if (buffer == nullptr
                && (free != mBuffers.end() || mBuffers.size() < mGrowthLimit)) {
            // We alloc before we free so failure leaves group unchanged.
            const size_t allocateSize = requestedSize < SIZE_MAX / 3 * 2 /* NB: ordering */ ?
                    requestedSize * 3 / 2 : requestedSize;
            buffer = new MediaBuffer(allocateSize);
            if (buffer->data() == nullptr) {
                ALOGE("Allocation failure for size %zu", allocateSize);
                delete buffer; // Invalid alloc, prefer not to call release.
                buffer = nullptr;
            } else {
                buffer->setObserver(this);
                if (free != mBuffers.end()) {
                    ALOGV("reallocate buffer, requested size %zu vs available %zu",
                            requestedSize, (*free)->size());
                    (*free)->setObserver(nullptr);
                    (*free)->release();
                    *free = buffer; // in-place replace
                } else {
                    ALOGV("allocate buffer, requested size %zu", requestedSize);
                    mBuffers.emplace_back(buffer);
                }
            }
        }
        if (buffer != nullptr) {
            buffer->add_ref();
            buffer->reset();
            *out = buffer;
            return OK;
        }
        if (nonBlocking) {
            *out = nullptr;
            return WOULD_BLOCK;
        }
        // All buffers are in use, block until one of them is returned.
        mCondition.wait(mLock);
    }
    // Never gets here.
}

void MediaBufferGroup::signalBufferReturned(MediaBuffer *) {
    mCondition.signal();
}

}  // namespace android
