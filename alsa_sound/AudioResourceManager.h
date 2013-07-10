/* AudioiResourceManager.h

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

#ifndef ALSA_SOUND_AUDIO_RESOURCE_MANAGER_H
#define ALSA_SOUND_AUDIO_RESOURCE_MANAGER_H

#include <stdint.h>
#include <sys/types.h>
#include <system/audio.h>
#include <cutils/misc.h>
#include <cutils/config_utils.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/String8.h>
#include <utils/threads.h>
#include <utils/List.h>
#include <hardware_legacy/AudioSystemLegacy.h>
#include <hardware/audio.h>
#include "AudioHardwareALSA.h"
#ifdef RESOURCE_MANAGER
namespace android_audio_legacy {

using android::List;
using android::Mutex;
using android::KeyedVector;
using android::status_t;
using android::Vector;
using android::String16;
using android::String8;

class AudioResourceManager;
class AudioHardwareALSA;
// 8x10 Resource Manager Changes
// UseCaseMetadata structure has the usecase and its
// enum value when  used with STRING_TO_ENUM
// UseCaseMetadata structure has the usecase and its
//  max supported refCount with STRING_AND_REFCOUNT
struct UseCaseMetadata {
    const char *name;
    uint32_t value;
};

#define STRING_TO_ENUM(string) { #string, string }
#define STRING_AND_REFCOUNT(string, refCount) { #string, refCount }
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

const struct UseCaseMetadata sUseCaseNameToEnumValue[] = {
    STRING_TO_ENUM(USECASE_PCM_PLAYBACK),
    STRING_TO_ENUM(USECASE_PCM_RECORDING),
    STRING_TO_ENUM(USECASE_NON_TUNNEL_DSP_PLAYBACK),
    STRING_TO_ENUM(USECASE_TUNNEL_DSP_PLAYBACK),
    STRING_TO_ENUM(USECASE_LPA_PLAYBACK),
    STRING_TO_ENUM(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK),
    STRING_TO_ENUM(USECASE_VIDEO_PLAYBACK),
    STRING_TO_ENUM(USECASE_VIDEO_RECORD),
    STRING_TO_ENUM(USECASE_VOICE_CALL),
    STRING_TO_ENUM(USECASE_VOIP_CALL),
    STRING_TO_ENUM(USECASE_VIDEO_TELEPHONY),
    STRING_TO_ENUM(USECASE_FM_PLAYBACK),
    STRING_TO_ENUM(USECASE_ULL),
};

//TODO: Push these dependency to a text file.

// The below structures represent the conflicting usecases for
// each incoming usecase. The STRING_AND_REFCOUNT consists of
// the usecase and the number of instances of a particular
// usecase that could trigger a failure of incoming usecase.
const struct UseCaseMetadata sTunnelPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_TUNNEL_DSP_PLAYBACK, 1),
    STRING_AND_REFCOUNT(USECASE_LPA_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK, 1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sLPAPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_TUNNEL_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_LPA_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sPcmPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sPcmRecordingConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,2),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sNonTunnelDSPPlaybackConflicts[] = {
   STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_DSP_PLAYBACK,2),
   STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,2),
   STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
   STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
   STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sNonTunnelVideoDSPPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_DSP_PLAYBACK,2),
    STRING_AND_REFCOUNT(USECASE_LPA_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_TUNNEL_DSP_PLAYBACK,1),
    /* Commenting video usecases - video - video concurrency will be handled by
     * video team. Failing here would be risky. Assuming, a video set parameter
     * comes we do not have any video only usecases running.
     */
    //STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};
const struct UseCaseMetadata sVideoPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_LPA_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_TUNNEL_DSP_PLAYBACK,1),
    /* Commenting video usecases - video - video concurrency will be handled by
     * video team. Failing here would be risky. Assuming, a video set parameter
     * comes we do not have any video only usecases running.
     */
    //STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

const struct UseCaseMetadata sVideoRecordConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_PCM_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_PCM_RECORDING,2),
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_TUNNEL_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VOICE_CALL,1),
    STRING_AND_REFCOUNT(USECASE_VOIP_CALL,1),
    /* Commenting video usecases - video - video concurrency will be handled by
     * video team. Failing here would be risky. Assuming, a video set parameter
     * comes we do not have any video only usecases running.
     */
    //STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    //STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
    //STRING_AND_REFCOUNT(USECASE_LPA_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_FM_PLAYBACK,1),
};

// No conflicts for voice call. Voice call has to be honoured
// even if any other use case is running. This will be done
// even though there is dsp constraint.
const struct UseCaseMetadata sVoiceCallConflicts[] = {
};

const struct UseCaseMetadata sVoipCallConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_VOICE_CALL, 2),
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

//Video telephony to be treated as vide decode/encode.
/*
const struct UseCaseMetadata sVideoTelephonyConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_VOICE_CALL,2),
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};
*/
const struct UseCaseMetadata sFMPlaybackConflicts[] = {
    STRING_AND_REFCOUNT(USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_PLAYBACK,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_RECORD,1),
    STRING_AND_REFCOUNT(USECASE_VIDEO_TELEPHONY,1),
};

class AudioResourceManager {

// Utilities related to  handling concurrency
public:
    AudioResourceManager(AudioHardwareALSA *parent);

    virtual            ~AudioResourceManager();

    /**
     * check to see if the AudioResourceManager has been initialized.
     * return status based on values defined in include/utils/Errors.h
     */
   status_t    initCheck();

   status_t setParameter(const String8& keyValuePairs);

private:

   status_t updateDependencyListForUsecases(audio_use_case_value_t value);

   status_t extractMetaData(const struct UseCaseMetadata *table, size_t size,
       const char *name);

   status_t handleConcurrency(audio_use_case_value_t useCaseString, bool value);
   status_t updateUsecaseRefCount(audio_use_case_value_t useCase, bool value);
   status_t  checkUseCaseConflict(audio_use_case_value_t useCase);

   struct ConcurrencyDataDescriptor {
       audio_use_case_value_t useCase;
       uint32_t refCount;
   };

   struct ConcurrencyRefCountVector {
       ConcurrencyDataDescriptor * desc;
       uint32_t maxRefCount;
   };

   typedef List <ConcurrencyRefCountVector *> ConcurrencyDataList;

   ConcurrencyDataList mConcurrencyDataList[ARRAY_SIZE(sUseCaseNameToEnumValue)];
   //Vector stores the reference count corresponding to each usecase

   KeyedVector<audio_use_case_value_t, ConcurrencyDataDescriptor *> mConcurrencyDataVector;

   KeyedVector<audio_use_case_value_t, ConcurrencyDataList > mConcurrencyDataGraph;

   Mutex mLock;
   status_t mStatus;
};

};
#endif
#endif /* ALSA_SOUND_AUDIO_RESOURCE_MANAGER_H */
