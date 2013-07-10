/* AudioResourceManager.cpp

Copyright (c) 2013, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#define LOG_TAG "AudioResourceManager"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include "AudioResourceManager.h"

namespace android_audio_legacy{

AudioResourceManager::AudioResourceManager(AudioHardwareALSA *parent) {

    //update vector , init ref count to zero
    mStatus  = NO_ERROR;
    ALOGD("ssize of use case = %d",ARRAY_SIZE(sUseCaseNameToEnumValue));
    for(uint8_t i=0; i < ARRAY_SIZE(sUseCaseNameToEnumValue); i++) {
        ConcurrencyDataDescriptor *desc =  new ConcurrencyDataDescriptor;
        desc->useCase = (audio_use_case_value_t)i;
        desc->refCount = 0;
        mConcurrencyDataVector.add(desc->useCase,desc);
        ALOGD("desc = %p, desc->useCase = %d,desc->refCount = %d",
                    desc,  desc->useCase,desc->refCount);
    }

    for(uint8_t i=0; i < ARRAY_SIZE(sUseCaseNameToEnumValue); i++) {
        mStatus = updateDependencyListForUsecases((audio_use_case_value_t)i);
        if(mStatus != OK) {
            break;
        }
    }

#if 0
    //Test Code for checking the  graph creation
    for(uint8_t i=0; i < ARRAY_SIZE(sUseCaseNameToEnumValue); i++) {
        ALOGV("### usecase = %d", i);
        ConcurrencyDataList list =
                mConcurrencyDataGraph.valueFor((audio_use_case_value_t)i);

        for (ConcurrencyDataList::iterator it = list.begin(); it != list.end(); ++it) {
            ConcurrencyRefCountVector *vector = (*it);
            ALOGV("### vector = %p,vector->desc =%p,vector->desc->usecase = %d",
                    vector, vector->desc,vector->desc->useCase);
        }
    }
#endif
}

AudioResourceManager::~AudioResourceManager() {

    //TODO : Do we need to free from graph vector as they point to the same one
    // check that
    ALOGD("Destructor : ssize of graph  = %d", mConcurrencyDataGraph.size());
    for (size_t i = 0; i < mConcurrencyDataGraph.size();) {
        ConcurrencyDataList list = mConcurrencyDataGraph.valueAt(i);
        while(!list.empty()) {
            ConcurrencyDataList::iterator it = list.begin();
            ConcurrencyRefCountVector *vector = (*it);
            ALOGD(" vector = %p, vector->desc = %p", vector, vector->desc);
            vector->desc = NULL;
            delete vector;
            vector = NULL;
            list.erase(it);
        }
        mConcurrencyDataGraph.removeItemsAt(i);
    }
    ALOGV("mConcurrencyDataGraph.size()- after removal %d ",
                mConcurrencyDataGraph.size());
    ALOGV("mConcurrencyDataVector.size() = %d", mConcurrencyDataVector.size());
    for (size_t i = 0; i < mConcurrencyDataVector.size(); i++) {
        ALOGV("desc = %p",mConcurrencyDataVector.valueAt(i));
        delete mConcurrencyDataVector.valueAt(i);
    }
    ALOGV("After delete vector mConcurrencyDataVector.size() = %d",
                mConcurrencyDataVector.size());

    for (size_t i = 0; i < mConcurrencyDataVector.size();) {
        mConcurrencyDataVector.removeItemsAt(i);
    }

    ALOGV("After removal mConcurrencyDataVector.size() = %d",
                mConcurrencyDataVector.size());
}

status_t AudioResourceManager::initCheck() {
    return mStatus;
}

status_t AudioResourceManager::setParameter(const String8& keyValuePairs) {

    String8 key;
    String8 value;
    status_t status = NO_ERROR;
    AudioParameter param = AudioParameter(keyValuePairs);

    Mutex::Autolock autolock(mLock);

    ALOGD(" setParameter %s:",  keyValuePairs.string());
    for(uint8_t i=0; i < ARRAY_SIZE(sUseCaseNameToEnumValue); i++) {
        key = sUseCaseNameToEnumValue[i].name;
        if (param.get(key, value) == NO_ERROR) {

            audio_use_case_value_t useCase =
                        (audio_use_case_value_t) extractMetaData(
                    sUseCaseNameToEnumValue,
                    ARRAY_SIZE(sUseCaseNameToEnumValue),
                    key);
            ALOGD("key = %s, value = %s, useCase = %d",
                        key.string(), value.string(), (int32_t)useCase);
            if(value == "true") {
                // check if there is a conflicting usecase
                // if yes, return error without updating the refCount
                // if no increment the refCount
                ALOGV("handleConcurrency");
                status = handleConcurrency(useCase, true);
            }
            else if(value == "false") {
                // Decrement the refCount
                ALOGV("updateUsecaseRefCount");
                status = updateUsecaseRefCount(useCase, false);
            }
            else {
                ALOGE(" Wrong value set for use case = %s", key.string());
                status = BAD_VALUE;
            }
            break;
        } else {
            status = NAME_NOT_FOUND;
            ALOGV("Not a concurrency setParameter - Not an error");

        }
    }
    param.remove(key);

    return status;
}

status_t AudioResourceManager::updateDependencyListForUsecases(
        audio_use_case_value_t useCase) {
  //Read the list from predefined as they are dependent
    uint32_t size = 0;
    struct UseCaseMetadata *table = NULL;
    status_t err =  NO_ERROR;
    audio_use_case_value_t dependentUseCase = (audio_use_case_value_t)0;
    ConcurrencyDataDescriptor *desc = NULL;

    switch(useCase) {
       case USECASE_PCM_PLAYBACK:
           table = (UseCaseMetadata * )sPcmPlaybackConflicts;
           size = ARRAY_SIZE(sPcmPlaybackConflicts);
           break;
       case USECASE_PCM_RECORDING:
           table = (UseCaseMetadata * )sPcmRecordingConflicts;
           size = ARRAY_SIZE(sPcmRecordingConflicts);
           break;
       case USECASE_NON_TUNNEL_DSP_PLAYBACK:
           table = (UseCaseMetadata * )sNonTunnelDSPPlaybackConflicts;
           size = ARRAY_SIZE(sNonTunnelDSPPlaybackConflicts);
           break;
       case USECASE_TUNNEL_DSP_PLAYBACK:
           table = (UseCaseMetadata * )sTunnelPlaybackConflicts;
           size = ARRAY_SIZE(sTunnelPlaybackConflicts);
           break;
       case USECASE_LPA_PLAYBACK:
           table = (UseCaseMetadata * )sLPAPlaybackConflicts;
           size = ARRAY_SIZE(sLPAPlaybackConflicts);
           break;
       case USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK:
           table = (UseCaseMetadata * )sNonTunnelVideoDSPPlaybackConflicts;
           size = ARRAY_SIZE(sNonTunnelVideoDSPPlaybackConflicts);
           break;
       case USECASE_VIDEO_PLAYBACK:
           table = (UseCaseMetadata * )sVideoPlaybackConflicts;
           size = ARRAY_SIZE(sVideoPlaybackConflicts);
           break;
       case USECASE_VIDEO_RECORD:
           table = (UseCaseMetadata * )sVideoRecordConflicts;
           size = ARRAY_SIZE(sVideoRecordConflicts);
           break;
        case USECASE_VOICE_CALL:
           table = (UseCaseMetadata * )sVoiceCallConflicts;
           size = ARRAY_SIZE(sVoiceCallConflicts);
           break;
        case USECASE_VOIP_CALL:
           table = (UseCaseMetadata * )sVoipCallConflicts;
           size = ARRAY_SIZE(sVoipCallConflicts);
           break;
       case USECASE_VIDEO_TELEPHONY:
           //table = (UseCaseMetadata * )sVideoTelephonyConflicts;
           //size = ARRAY_SIZE(sVideoTelephonyConflicts);
           break;
       case USECASE_FM_PLAYBACK:
           table = (UseCaseMetadata * )sFMPlaybackConflicts;
           size = ARRAY_SIZE(sFMPlaybackConflicts);
           break;
       case USECASE_ULL:
           ALOGD("USECASE_ULL");
           break;
       default:
           ALOGE("BAD_VALUE");
           err = BAD_VALUE;
           break;
    }

    ALOGD("updateDependencyListForUsecases = %d,  table = %p,size = %d",
               useCase, table, size);
    if(!err) {
        for(uint8_t i = 0; i < size; i++) {
            dependentUseCase = (audio_use_case_value_t) extractMetaData(
                    sUseCaseNameToEnumValue,
                    ARRAY_SIZE(sUseCaseNameToEnumValue),table[i].name);
            desc = mConcurrencyDataVector.valueFor(dependentUseCase);
            ConcurrencyRefCountVector *vector = new ConcurrencyRefCountVector();
            vector->desc = desc;
            vector->maxRefCount = table[i].value;
            ALOGD("desc = %p, dependentUseCase = %d, vector = %p",
                        desc, dependentUseCase, vector);

            mConcurrencyDataList[useCase].push_back(vector);
        }
        mConcurrencyDataGraph.add(useCase, mConcurrencyDataList[useCase]);
    }

    return err;
}

status_t AudioResourceManager::handleConcurrency(
            audio_use_case_value_t useCase, bool value) {

    status_t err = NO_ERROR;
    switch(useCase) {
        case USECASE_PCM_PLAYBACK:
        case USECASE_PCM_RECORDING:
        case USECASE_NON_TUNNEL_DSP_PLAYBACK:
        case USECASE_TUNNEL_DSP_PLAYBACK:
        case USECASE_VOIP_CALL:
        case USECASE_VOICE_CALL:
        case USECASE_VIDEO_PLAYBACK:
        case USECASE_VIDEO_RECORD:
        case USECASE_VIDEO_TELEPHONY:
        case USECASE_LPA_PLAYBACK:
        case USECASE_FM_PLAYBACK:
        case USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK:
            err = checkUseCaseConflict(useCase);
            break;
        case USECASE_ULL:
            return err;
        default:
            ALOGE("Invalid usecase");
            err = BAD_VALUE;
            break;
    }
    if(!err) {
        updateUsecaseRefCount(useCase, value);
    }
    else {
        ALOGV("Concurrency Not supported for usecase = %d", useCase);
    }

    return err;
}

status_t AudioResourceManager::checkUseCaseConflict(
        audio_use_case_value_t useCase) {

    status_t err = NO_ERROR;
    ALOGD("checkUseCaseConflict = %d", useCase);
    if (!mConcurrencyDataGraph.isEmpty()) {
        ConcurrencyDataList list = mConcurrencyDataGraph.valueFor(useCase);
        for (ConcurrencyDataList::iterator it = list.begin(); it != list.end(); ++it) {
            ConcurrencyRefCountVector *vector = (*it);
            ALOGD("usecase = %d , refCount  =%d, maxrefcount =%d",
                    vector->desc->useCase, vector->desc->refCount,
                    vector->maxRefCount);
            if(vector->desc->refCount < vector->maxRefCount) {
                continue;
            } else {
                err = INVALID_OPERATION;
                break;
            }
        }
    }
    return err;
}

status_t AudioResourceManager::extractMetaData(
        const struct UseCaseMetadata *table, size_t size,const char *name) {

    for (size_t i = 0; i < size; i++) {
        if (strcmp(table[i].name, name) == 0) {
            ALOGV("extractMetaData  found %s, table[i].value = %d",
                                     table[i].name, table[i].value);
            return table[i].value;
        }
    }
    return BAD_VALUE;
}

status_t AudioResourceManager::updateUsecaseRefCount(
        audio_use_case_value_t useCase, bool value) {

    if (!mConcurrencyDataVector.isEmpty()) {
        ConcurrencyDataDescriptor *desc =
                    mConcurrencyDataVector.valueFor(useCase);
        ALOGD("updateUsecaseRefCount useCase=%d,value=%d,desc->refCount=%d",
                    useCase, value, desc->refCount);
        if(value == false) {
            if(desc->refCount)
                desc->refCount--;
        } else {
            desc->refCount++;
        }
        ALOGD("updateUsecaseRefCount useCase = %d, refCount =%d",
                    useCase, desc->refCount);
    }
    return NO_ERROR;
}

}
