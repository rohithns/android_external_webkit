/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TexturesGenerator.h"

#if USE(ACCELERATED_COMPOSITING)

#include "BaseLayerAndroid.h"
#include "GLUtils.h"
#include "PaintLayerOperation.h"
#include "TilesManager.h"

#ifdef DEBUG

#include <cutils/log.h>
#include <wtf/CurrentTime.h>
#include <wtf/text/CString.h>

#undef XLOG
#define XLOG(...) android_printLog(ANDROID_LOG_DEBUG, "TexturesGenerator", __VA_ARGS__)

#else

#undef XLOG
#define XLOG(...)

#endif // DEBUG

namespace WebCore {

void TexturesGenerator::scheduleOperation(QueuedOperation* operation)
{
    android::Mutex::Autolock lock(mRequestedOperationsLock);
    for (unsigned int i = 0; i < mRequestedOperations.size(); i++) {
        QueuedOperation** s = &mRequestedOperations[i];
        // A similar operation is already in the queue. The newer operation may
        // have additional dirty tiles so delete the existing operation and
        // replace it with the new one.
        if (*s && *s == operation) {
            QueuedOperation* oldOperation = *s;
            *s = operation;
            delete oldOperation;
            return;
        }
    }

    mRequestedOperations.append(operation);
    m_newRequestLock.lock();
    m_newRequestCond.signal();
    m_newRequestLock.unlock();
}

void TexturesGenerator::removeOperationsForPage(TiledPage* page)
{
    removeOperationsForFilter(new PageFilter(page));
}

void TexturesGenerator::removeOperationsForBaseLayer(BaseLayerAndroid* layer)
{
    removeOperationsForFilter(new PaintLayerFilter(layer));
}

void TexturesGenerator::removeOperationsForFilter(OperationFilter* filter)
{
    mRequestedOperationsLock.lock();
    for (unsigned int i = 0; i < mRequestedOperations.size();) {
        QueuedOperation* operation = mRequestedOperations[i];
        if (filter->check(operation)) {
            mRequestedOperations.remove(i);
            delete operation;
        } else {
            i++;
        }
    }

    QueuedOperation* operation = m_currentOperation;
    if (operation && filter->check(operation))
        m_waitForCompletion = true;

    mRequestedOperationsLock.unlock();
    delete filter;

    if (!m_waitForCompletion)
        return;

    // At this point, it means that we are currently painting a operation that
    // we want to be removed -- we should wait until it is painted, so that
    // when we return our caller can be sure that there is no more TileSet
    // in the queue for that TiledPage and can safely deallocate the BaseTiles.
    mRequestedOperationsLock.lock();
    mRequestedOperationsCond.wait(mRequestedOperationsLock);
    m_waitForCompletion = false;
    mRequestedOperationsLock.unlock();
}

status_t TexturesGenerator::readyToRun()
{
    TilesManager::instance()->enableTextures();
    XLOG("Textures enabled (context acquired...)");
    TilesManager::instance()->paintTexturesDefault();
    XLOG("Textures painted");
    TilesManager::instance()->markGeneratorAsReady();
    XLOG("Thread ready to run");
    return NO_ERROR;
}

bool TexturesGenerator::threadLoop()
{
    mRequestedOperationsLock.lock();

    if (!mRequestedOperations.size()) {
        XLOG("threadLoop, waiting for signal");
        m_newRequestLock.lock();
        mRequestedOperationsLock.unlock();
        m_newRequestCond.wait(m_newRequestLock);
        m_newRequestLock.unlock();
        XLOG("threadLoop, got signal");
    } else {
        XLOG("threadLoop going as we already have something in the queue");
        mRequestedOperationsLock.unlock();
    }

    m_currentOperation = 0;
    bool stop = false;
    while (!stop) {
        XLOG("threadLoop evaluating the requests");
        mRequestedOperationsLock.lock();
        if (mRequestedOperations.size()) {
            m_currentOperation = mRequestedOperations.first();
            mRequestedOperations.remove(0);
            XLOG("threadLoop, popping the first request (%d requests left)",
                 mRequestedOperations.size());
        }
        mRequestedOperationsLock.unlock();

        if (m_currentOperation) {
            XLOG("threadLoop, painting the request");
            m_currentOperation->run();
            XLOG("threadLoop, painting the request - DONE");
        }

        mRequestedOperationsLock.lock();
        if (m_currentOperation) {
            delete m_currentOperation;
            m_currentOperation = 0;
            mRequestedOperationsCond.signal();
        }
        if (!mRequestedOperations.size())
            stop = true;
        if (m_waitForCompletion)
            mRequestedOperationsCond.signal();
        mRequestedOperationsLock.unlock();
    }

    return true;
}

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING)