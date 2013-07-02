/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
 ** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioHardwareALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>
#include <audio_utils/resampler.h>
#include <pthread.h>

#include "AudioHardwareALSA.h"
#ifdef QCOM_USBAUDIO_ENABLED
#include "AudioUsbALSA.h"
#endif
#include "AudioUtil.h"

//#define OUTPUT_BUFFER_LOG
#ifdef OUTPUT_BUFFER_LOG
    FILE *outputBufferFile1;
    char outputfilename [50] = "/data/output_proxy";
    static int number = 0;
#endif

extern "C"
{
    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android_audio_legacy::AudioHardwareInterface *createAudioHardware(void) {
        return android_audio_legacy::AudioHardwareALSA::create();
    }
#ifdef QCOM_ACDB_ENABLED
    static int (*acdb_init)();
    static void (*acdb_deallocate)();
#endif
#ifdef QCOM_CSDCLIENT_ENABLED
    static int (*csd_client_init)();
    static int (*csd_client_deinit)();
    static int (*csd_start_playback)(uint32_t);
    static int (*csd_stop_playback)(uint32_t);
    static int (*csd_standby_voice)(uint32_t);
    static int (*csd_resume_voice)(uint32_t);
#endif
}         // extern "C"

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {

    AudioHardwareInterface * hardwareInterface = new AudioHardwareALSA();
    if(hardwareInterface->initCheck() != OK) {
        ALOGE("NULL - AHAL creation failed");
        delete hardwareInterface;
        hardwareInterface = NULL;
    }
    return hardwareInterface;
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0),mVoipInStreamCount(0),mVoipOutStreamCount(0),mVoipMicMute(false),
    mVoipBitRate(0),mCallState(0),mAcdbHandle(NULL),mCsdHandle(NULL),mMicMute(0)
{
    FILE *fp;
    char soundCardInfo[200];
    char platform[128], baseband[128], platformVer[128], audioInit[128];
    int verNum = 0;
    struct snd_ctl_card_info *cardInfo;

    mDeviceList.clear();
    mVoiceCallState = CALL_INACTIVE;
    mVolteCallState = CALL_INACTIVE;
    mVoice2CallState = CALL_INACTIVE;
    mVSID = 0;
    mIsFmActive = 0;
    mDevSettingsFlag = 0;
    bool audioInitDone = false;
    int sleepRetry = 0;
#ifdef QCOM_USBAUDIO_ENABLED
    mAudioUsbALSA = new AudioUsbALSA();
    musbPlaybackState = 0;
    musbRecordingState = 0;
#endif
#ifdef USES_FLUENCE_INCALL
    mDevSettingsFlag |= TTY_OFF | DMIC_FLAG;
#else
    mDevSettingsFlag |= TTY_OFF;
#endif
    mBluetoothVGS = false;
    mFusion3Platform = false;
    //mIsVoicePathActive = false;

    mRouteAudioToExtOut = false;
    mA2dpDevice = NULL;
    mA2dpStream = NULL;
    mUsbDevice = NULL;
    mUsbStream = NULL;
    mExtOutStream = NULL;
    mResampler = NULL;
    mExtOutActiveUseCases = USECASE_NONE;
    mIsExtOutEnabled = false;
    mKillExtOutThread = false;
    mExtOutThreadAlive = false;
    mExtOutThread = NULL;
    mUcMgr = NULL;
    mCanOpenProxy=1;

#ifdef QCOM_ACDB_ENABLED
    acdb_deallocate = NULL;
#endif

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    mListenHw == NULL;
#endif

    mALSADevice = new ALSADevice();
    if (!mALSADevice) {
        mStatus = NO_INIT;
        return;
    }
    if (mALSADevice->initCheck() != OK) {
        mStatus = NO_INIT;
        return;
    }

#ifdef QCOM_ACDB_ENABLED
    mAcdbHandle = ::dlopen("/vendor/lib/libacdbloader.so", RTLD_NOW);
    if (mAcdbHandle == NULL) {
        ALOGE("AudioHardware: DLOPEN not successful for ACDBLOADER");
    } else {
        ALOGD("AudioHardware: DLOPEN successful for ACDBLOADER");
        acdb_init = (int (*)())::dlsym(mAcdbHandle,"acdb_loader_init_ACDB");
        if (acdb_init == NULL) {
            ALOGE("dlsym:Error:%s Loading acdb_loader_init_ACDB");
        }else {
           acdb_init();
           acdb_deallocate = (void (*)())::dlsym(mAcdbHandle,"acdb_loader_deallocate_ACDB");
        }
    }
    mALSADevice->setACDBHandle(mAcdbHandle);
#endif

    cardInfo = mALSADevice->getSoundCardInfo();

#ifdef QCOM_USBAUDIO_ENABLED
    if (mAudioUsbALSA) {
        mAudioUsbALSA->setProxySoundCard(cardInfo->card);
    }
#endif

    while (audioInitDone == false && sleepRetry < MAX_SLEEP_RETRY) {
        property_get("qcom.audio.init", audioInit, NULL);
        ALOGD("qcom.audio.init is set to %s\n",audioInit);
        if (!strncmp(audioInit, "complete", sizeof("complete"))) {
            audioInitDone = true;
        } else {
            ALOGD("Sleeping for 50 ms");
            usleep(AUDIO_INIT_SLEEP_WAIT*1000);
            sleepRetry++;
        }
    }

    if (!strcmp((const char*)cardInfo->name, "msm8974-taiko-mtp-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Taiko", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8974-taiko-cdp-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Taiko_CDP", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8974-taiko-fluid-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Taiko_Fluid", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8974-taiko-liquid-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Taiko_liquid", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "apq8074-taiko-db-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_apq_Taiko_DB", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8x10-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_8x10_wcd", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8930-sitar-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Sitar", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8960-tabla1x-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8226-tapan-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Tapan", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8226-tapan-skuf-snd-card")) {
        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_Tapan_SKUF", cardInfo->card);
    } else if (!strcmp((const char*)cardInfo->name, "msm8960-tabla1x-snd-card") ||
               !strcmp((const char*)cardInfo->name, "apq8064-tabla-snd-card") ||
               !strcmp((const char*)cardInfo->name, "msm8960-snd-card") ||
               !strcmp((const char*)cardInfo->name, "msm-snd-card")) {
        property_get("ro.board.platform", platform, "");
        property_get("ro.baseband", baseband, "");
        if (!strcmp("msm8960", platform) &&
            (!strcmp("mdm", baseband) || !strcmp("sglte2", baseband))) {
            ALOGD("Detected Fusion tabla 2.x");
            mFusion3Platform = true;
            if((fp = fopen("/sys/devices/system/soc/soc0/platform_version","r")) == NULL) {
                ALOGE("Cannot open /sys/devices/system/soc/soc0/platform_version file");

                snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_2x_Fusion3", cardInfo->card);
            } else {
                while((fgets(platformVer, sizeof(platformVer), fp) != NULL)) {
                    ALOGD("platformVer %s", platformVer);

                    verNum = atoi(platformVer);
                    if (verNum == 0x10001) {
                        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_I2SFusion", cardInfo->card);
                        break;
                    } else {
                        snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_2x_Fusion3", cardInfo->card);
                        break;
                    }
                }
            }
            fclose(fp);
        } else {
            ALOGD("Detected tabla 2.x sound card");
            snd_use_case_mgr_create(&mUcMgr, "snd_soc_msm_2x", cardInfo->card);
        }
    }

    if (mUcMgr == NULL) {
        mStatus = NO_INIT;
        return;
    }

#ifdef QCOM_CSDCLIENT_ENABLED
    if (mFusion3Platform) {
        mCsdHandle = ::dlopen("/vendor/lib/libcsd-client.so", RTLD_NOW);
        if (mCsdHandle == NULL) {
            ALOGE("AudioHardware: DLOPEN not successful for CSD CLIENT");
        } else {
            ALOGD("AudioHardware: DLOPEN successful for CSD CLIENT");
            csd_client_init = (int (*)())::dlsym(mCsdHandle, "csd_client_init");
            csd_client_deinit = (int (*)())::dlsym(mCsdHandle,
                                                   "csd_client_deinit");
            csd_start_playback = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                   "csd_client_start_playback");
            csd_stop_playback = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                    "csd_client_stop_playback");
            csd_standby_voice = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                    "csd_client_standby_voice");
            csd_resume_voice = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                     "csd_client_resume_voice");

            if (csd_client_init == NULL) {
                ALOGE("csd_client_init is NULL");
            } else {
                csd_client_init();
            }

        }
        mALSADevice->setCsdHandle(mCsdHandle);
    }
#endif

    if (mUcMgr < 0) {
        ALOGE("Failed to open ucm instance: %d", errno);
        mStatus = NO_INIT;
        return;
    } else {
        ALOGI("ucm instance opened: %u", (unsigned)mUcMgr);
        mUcMgr->isFusion3Platform = mFusion3Platform;
    }

    //set default AudioParameters
    AudioParameter param;
    String8 key;
    String8 value;
#ifdef QCOM_FLUENCE_ENABLED
    //Set default AudioParameter for fluencetype
    key  = String8(AUDIO_PARAMETER_KEY_FLUENCE_TYPE);
    property_get("ro.qc.sdk.audio.fluencetype",mFluenceKey,"0");
    if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro"))) {
        mDevSettingsFlag |= QMIC_FLAG;
        mDevSettingsFlag &= (~DMIC_FLAG);
        value = String8("fluencepro");
        ALOGD("FluencePro quadMic feature Enabled");
    } else if (0 == strncmp("fluence", mFluenceKey, sizeof("fluence"))) {
        mDevSettingsFlag |= DMIC_FLAG;
        mDevSettingsFlag &= (~QMIC_FLAG);
        value = String8("fluence");
        ALOGD("Fluence dualmic feature Enabled");
    } else if (0 == strncmp("none", mFluenceKey, sizeof("none"))) {
        mDevSettingsFlag &= (~DMIC_FLAG);
        mDevSettingsFlag &= (~QMIC_FLAG);
        value = String8("none");
        ALOGD("Fluence feature Disabled");
    }
    param.add(key, value);
    mALSADevice->setFlags(mDevSettingsFlag);
#endif

#ifdef QCOM_SSR_ENABLED
    //set default AudioParameters for surround sound recording
    char ssr_enabled[6] = "false";
    property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
    if (!strncmp("true", ssr_enabled, 4)) {
        ALOGD("surround sound recording is supported");
        param.add(String8(AUDIO_PARAMETER_KEY_SSR), String8("true"));
    } else {
        ALOGD("surround sound recording is not supported");
        param.add(String8(AUDIO_PARAMETER_KEY_SSR), String8("false"));
    }
#endif

    mStatus = OK;
    char spkr_prot_enabled[80] = "false";
    property_get("persist.speaker.prot.enable",spkr_prot_enabled,"0");
    if (!strncmp("true", spkr_prot_enabled, 4)) {
        ALOGD("Speaker Protection enabled");
        mspkrProtection.initialize(this);
        mALSADevice->setSpkrProtHandle(&mspkrProtection);
    } else
        ALOGD("Speaker Protection disabled");

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    mListenHw = new ListenHardware(mUcMgr, mAcdbHandle);
    if (mListenHw == NULL) {
        ALOGE("Failed to create ListenHardware");
    }
    else {
        if (mListenHw->init() != NO_ERROR) {
            ALOGE("Failed Init ListenHardware");
            delete mListenHw;
            mListenHw = NULL;
        }
    }
#endif
    mTunnelsUsed = 0;
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mUcMgr != NULL) {
        ALOGD("closing ucm instance: %u", (unsigned)mUcMgr);
        snd_use_case_mgr_close(mUcMgr);
    }
    if (mALSADevice) {
        delete mALSADevice;
    }
    for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
        it->useCase[0] = 0;
        mDeviceList.erase(it);
    }
    if (mResampler) {
        release_resampler(mResampler);
        mResampler = NULL;
    }
#ifdef QCOM_ACDB_ENABLED
     if (acdb_deallocate == NULL) {
        ALOGE("acdb_deallocate_ACDB is NULL");
     } else {
        acdb_deallocate();
     }
     if (mAcdbHandle) {
        ::dlclose(mAcdbHandle);
        mAcdbHandle = NULL;
     }
#endif
#ifdef QCOM_USBAUDIO_ENABLED
    delete mAudioUsbALSA;
#endif

#ifdef QCOM_CSDCLEINT_ENABLED
    if (mFusion3Platform) {
        if (mCsdHandle) {
            if (csd_client_deinit == NULL) {
                ALOGE("csd_client_deinit is NULL");
            } else {
                csd_client_deinit();
            }
            ::dlclose(mCsdHandle);
            mCsdHandle = NULL;
        }
    }
#endif
#ifdef QCOM_LISTEN_FEATURE_ENABLE
    if (mListenHw) {
        delete mListenHw;
        mListenHw = NULL;
    }
#endif
}
char* AudioHardwareALSA::getTunnel(bool hifi) {
    char* ret = NULL;
    ALOGV("mTunnelsUsed: 0x%x", mTunnelsUsed);
    if (!(mTunnelsUsed & 0x1)) {
        mTunnelsUsed |= 0x1;
        ret = SND_USE_CASE_MOD_PLAY_TUNNEL;
        if (hifi) {
            ret = SND_USE_CASE_VERB_HIFI_TUNNEL;
        }
    } else if (!(mTunnelsUsed & 0x2)) {
        mTunnelsUsed |= 0x2;
        ret = SND_USE_CASE_MOD_PLAY_TUNNEL2;
        if (hifi) {
            ret = SND_USE_CASE_VERB_HIFI_TUNNEL2;
        }
    } else if (!(mTunnelsUsed & 0x4)) {
        mTunnelsUsed |= 0x4;
        ret = SND_USE_CASE_MOD_PLAY_TUNNEL3;
        if (hifi) {
            ret = SND_USE_CASE_VERB_HIFI_TUNNEL3;
        }
    } else if (!(mTunnelsUsed & 0x8)) {
        mTunnelsUsed |= 0x8;
        ret = SND_USE_CASE_MOD_PLAY_TUNNEL4;
        if (hifi) {
            ret = SND_USE_CASE_VERB_HIFI_TUNNEL4;
        }
    }
    ALOGV("Tunnel utilized: %s", ret);
    return ret;
}
void AudioHardwareALSA::freeTunnel(char* useCase) {
    if(!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL)) ||
        !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                 MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL))) {
        mTunnelsUsed &= ~0x1;
    } else if(!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                       MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2)) ||
              !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                       MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2))) {
        mTunnelsUsed &= ~0x2;
    } else if(!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3,
                       MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3)) ||
              !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL3,
                       MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL3))) {
        mTunnelsUsed &= ~0x4;
    } else if(!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL4,
                       MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL4)) ||
              !strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL4,
                       MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL4))) {
        mTunnelsUsed &= ~0x8;
    }
    ALOGV("Tunnel freed: %s", useCase);
}

status_t AudioHardwareALSA::initCheck()
{
    return mStatus;
}

status_t AudioHardwareALSA::setVoiceVolume(float v)
{
    ALOGD("setVoiceVolume(%f)\n", v);
    if (v < 0.0) {
        ALOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        ALOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int newMode = mode();
    ALOGV("setVoiceVolume  newMode %d",newMode);
    int vol = lrint(v * 100.0);

    // Voice volume levels from android are mapped to driver volume levels as follows.
    // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
    // So adjust the volume to get the correct volume index in driver
    vol = 100 - vol;

    if (mALSADevice) {
        if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
            mALSADevice->setVoipVolume(vol);
        } else if (newMode == AUDIO_MODE_IN_CALL){
               if (mVoiceCallState == CALL_ACTIVE)
                   mALSADevice->setVoiceVolume(vol);
               else if (mVoice2CallState == CALL_ACTIVE)
                   mALSADevice->setVoice2Volume(vol);
               if (mVolteCallState == CALL_ACTIVE)
                   mALSADevice->setVoLTEVolume(vol);
        }
    }

    return NO_ERROR;
}

#ifdef QCOM_FM_ENABLED
status_t  AudioHardwareALSA::setFmVolume(float value)
{
    Mutex::Autolock autoLock(mLock);
    status_t status = NO_ERROR;

    int vol;

    if (value < 0.0) {
        ALOGW("setFmVolume(%f) under 0.0, assuming 0.0\n", value);
        value = 0.0;
    } else if (value > 1.0) {
        ALOGW("setFmVolume(%f) over 1.0, assuming 1.0\n", value);
        value = 1.0;
    }
    vol  = lrint((value * 0x2000) + 0.5);

    ALOGV("setFmVolume(%f)\n", value);
    ALOGD("Setting FM volume to %d (available range is 0 to 0x2000)\n", vol);

    mALSADevice->setFmVolume(vol);

    return status;
}
#endif

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return NO_ERROR;
}

status_t AudioHardwareALSA::setMode(int mode)
{
    status_t status = NO_ERROR;

    ALOGV("%s() mode=%d mMode=%d", __func__, mode, mMode);

    if (mode != mMode) {
        status = AudioHardwareBase::setMode(mode);
    }

    if (mode == AUDIO_MODE_IN_CALL) {
        if (mCallState == CALL_INACTIVE) {
            ALOGV("%s() defaulting vsid and call state",__func__);
            mCallState = CALL_ACTIVE;
            mVSID = VOICE_SESSION_VSID;
        } else {
            ALOGV("%s no op",__func__);
        }
    } else if (mode == AUDIO_MODE_NORMAL) {
        mCallState = CALL_INACTIVE;
    }

    return status;
}

#ifdef QCOM_DS1_DOLBY_DDP
void AudioHardwareALSA::parseDDPParams(int ddp_dev, int ddp_ch_cap, AudioParameter *param)
{
    String8 key;
    String8 value;
    int ddp_val = 0;
    key = String8("ddp_maxoutchan");
    if (param->getInt(key, ddp_val) == NO_ERROR) {
        if(mALSADevice) {
            mALSADevice->updateDDPEndpTable(ddp_dev, ddp_ch_cap,
                                        PARAM_ID_MAX_OUTPUT_CHANNELS, ddp_val);
        }
        param->remove(key);
    }
    key = String8("ddp_outmode");
    if (param->getInt(key, ddp_val) == NO_ERROR) {
        if(mALSADevice) {
            mALSADevice->updateDDPEndpTable(ddp_dev, ddp_ch_cap,
                                        PARAM_ID_OUT_CTL_OUTMODE, ddp_val);
        }
        param->remove(key);
    }
    key = String8("ddp_outlfeon");
    if (param->getInt(key, ddp_val) == NO_ERROR) {
        if(mALSADevice) {
            mALSADevice->updateDDPEndpTable(ddp_dev, ddp_ch_cap,
                                        PARAM_ID_OUT_CTL_OUTLFE_ON, ddp_val);
        }
        param->remove(key);
    }
    key = String8("ddp_compmode");
    if (param->getInt(key, ddp_val) == NO_ERROR) {
        if(mALSADevice) {
            mALSADevice->updateDDPEndpTable(ddp_dev, ddp_ch_cap,
                                        PARAM_ID_OUT_CTL_COMPMODE,  ddp_val);
        }
        param->remove(key);
    }
    key = String8("ddp_stereomode");
    if (param->getInt(key, ddp_val) == NO_ERROR) {
        if(mALSADevice) {
            mALSADevice->updateDDPEndpTable(ddp_dev, ddp_ch_cap,
                                        PARAM_ID_OUT_CTL_STEREO_MODE, ddp_val);
        }
        param->remove(key);
    }

    if (ddp_dev == mCurDevice) {
        Mutex::Autolock autoLock(mLock);
        setDDPEndpParams(ddp_dev);
    }
}
#endif

bool AudioHardwareALSA::isAnyCallActive() {

    bool ret = false;

    if ((mVoiceCallState == CALL_ACTIVE) ||
        (mVoiceCallState == CALL_HOLD) ||
        (mVoiceCallState == CALL_LOCAL_HOLD) ||
        (mVolteCallState == CALL_ACTIVE) ||
        (mVolteCallState == CALL_HOLD) ||
        (mVolteCallState == CALL_LOCAL_HOLD) ||
        (mVoice2CallState == CALL_ACTIVE) ||
        (mVoice2CallState == CALL_HOLD) ||
        (mVoice2CallState == CALL_LOCAL_HOLD)) {
        ret = true;
    }
    ALOGV("%s() ret=%d", __func__, ret);

    return ret;
}

status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key;
    String8 value;
    status_t status = NO_ERROR;
    int device;
    int btRate;
    int state;
    int ddp_dev, ddp_ch_cap;
    enum call_state  call_state = CALL_INVALID;
    uint32_t vsid = 0;

    ALOGV("%s() ,%s", __func__, keyValuePairs.string());

#ifdef QCOM_ADSP_SSR_ENABLED
    key = String8(AUDIO_PARAMETER_KEY_ADSP_STATUS);
    if (param.get(key, value) == NO_ERROR) {
    #ifdef QCOM_LISTEN_FEATURE_ENABLE
        if (mListenHw) {
            status = mListenHw->setParameters(keyValuePairs);
        }
    #endif
       if (value == "ONLINE") {
           ALOGV("ADSP online set SSRcomplete");
           mALSADevice->mADSPState = ADSP_UP_AFTER_SSR;
           return status;
       }
       else if (value == "OFFLINE") {
           ALOGV("ADSP online re-set SSRcomplete");
           mALSADevice->mADSPState = ADSP_DOWN;
           if ( mRouteAudioToExtOut==true) {
               ALOGV("ADSP offline close EXT output");
               uint32_t activeUsecase = getExtOutActiveUseCases_l();
               stopPlaybackOnExtOut_l(activeUsecase);
           }
           return status;
       }
    }
#endif

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mDevSettingsFlag &= TTY_CLEAR;
        if (value == "full" || value == "tty_full") {
            mDevSettingsFlag |= TTY_FULL;
        } else if (value == "hco" || value == "tty_hco") {
            mDevSettingsFlag |= TTY_HCO;
        } else if (value == "vco" || value == "tty_vco") {
            mDevSettingsFlag |= TTY_VCO;
        } else {
            mDevSettingsFlag |= TTY_OFF;
        }
        ALOGI("Changed TTY Mode=%s", value.string());
        mALSADevice->setFlags(mDevSettingsFlag);
        if(mMode != AUDIO_MODE_IN_CALL){
           return NO_ERROR;
        }
        doRouting(0);
    }
#ifdef QCOM_FLUENCE_ENABLED
    key = String8(AUDIO_PARAMETER_KEY_FLUENCE_TYPE);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "quadmic") {
            //Allow changing fluence type to "quadmic" only when fluence type is fluencepro
            if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro"))) {
                mDevSettingsFlag |= QMIC_FLAG;
                mDevSettingsFlag &= (~DMIC_FLAG);
                ALOGV("Fluence quadMic feature Enabled");
            }
        } else if (value == "dualmic") {
            //Allow changing fluence type to "dualmic" only when fluence type is fluencepro or fluence
            if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro")) ||
                0 == strncmp("fluence", mFluenceKey, sizeof("fluence"))) {
                mDevSettingsFlag |= DMIC_FLAG;
                mDevSettingsFlag &= (~QMIC_FLAG);
                ALOGV("Fluence dualmic feature Enabled");
            }
        } else if (value == "none") {
            mDevSettingsFlag &= (~DMIC_FLAG);
            mDevSettingsFlag &= (~QMIC_FLAG);
            ALOGV("Fluence feature Disabled");
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }
#endif

#ifdef QCOM_CSDCLIENT_ENABLED
    if (mFusion3Platform) {
        key = String8(INCALLMUSIC_KEY);
        if (param.get(key, value) == NO_ERROR) {
            if (value == "true") {
                ALOGV("Enabling Incall Music setting in the setparameter\n");
                if (csd_start_playback == NULL) {
                    ALOGE("csd_client_start_playback is NULL");
                    } else {
                        csd_start_playback(ALL_SESSION_VSID);
                    }
            } else {
                ALOGV("Disabling Incall Music setting in the setparameter\n");
                if (csd_stop_playback == NULL) {
                    ALOGE("csd_client_stop_playback is NULL");
                } else {
                    csd_stop_playback(ALL_SESSION_VSID);
                }
            }
        }
    }
#endif

#ifdef QCOM_ANC_HEADSET_ENABLED
    key = String8(ANC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            ALOGV("Enabling ANC setting in the setparameter\n");
            mDevSettingsFlag |= ANC_FLAG;
        } else {
            ALOGV("Disabling ANC setting in the setparameter\n");
            mDevSettingsFlag &= (~ANC_FLAG);
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }
#endif

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device)
            doRouting(device);
        param.remove(key);
    }

    key = String8(BT_SAMPLERATE_KEY);
    if (param.getInt(key, btRate) == NO_ERROR) {
        mALSADevice->setBtscoRate(btRate);
        param.remove(key);
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "on") {
            mBluetoothVGS = true;
        } else {
            mBluetoothVGS = false;
        }
    }

    key = String8(WIDEVOICE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }

        if(mALSADevice) {
            mALSADevice->enableWideVoice(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

    key = String8("a2dp_connected");
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            status_t err = openExtOutput(AudioSystem::DEVICE_OUT_ALL_A2DP);
        } else {
            status_t err = closeExtOutput(AudioSystem::DEVICE_OUT_ALL_A2DP);
        }
        param.remove(key);
    }

    key = String8("A2dpSuspended");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpDevice != NULL) {
            mA2dpDevice->set_parameters(mA2dpDevice,keyValuePairs);
            if(value=="true"){
                 uint32_t activeUsecase = getExtOutActiveUseCases_l();
                 status_t err = suspendPlaybackOnExtOut_l(activeUsecase);
            }
        }
        param.remove(key);
    }

    key = String8("a2dp_sink_address");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpStream != NULL) {
            mA2dpStream->common.set_parameters(&mA2dpStream->common,keyValuePairs);
        }
        param.remove(key);
    }

    key = String8("usb_connected");
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            status_t err = openExtOutput(AUDIO_DEVICE_OUT_ALL_USB);
        } else {
            status_t err = closeExtOutput(AUDIO_DEVICE_OUT_ALL_USB);
        }
        param.remove(key);
    }
#ifdef QCOM_WFD_ENABLED
    key = String8("wfd_channel_cap");
    if (param.get(key, value) == NO_ERROR) {
        if(mALSADevice){
            mALSADevice->setWFDChannelCaps(atoi(value));
            ALOGV("Channel capability set");
        }
        param.remove(key);
    }
#endif
    key = String8("card");
    if (param.get(key, value) == NO_ERROR) {
        if (mUsbStream != NULL) {
            ALOGV("mUsbStream->common.set_parameters");
            mUsbStream->common.set_parameters(&mUsbStream->common,keyValuePairs);
        }
        param.remove(key);
    }

    key = String8(VOIPRATE_KEY);
    if (param.get(key, value) == NO_ERROR) {
            mVoipBitRate = atoi(value);
        param.remove(key);
    }

    key = String8(FENS_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableFENS(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

#ifdef QCOM_FM_ENABLED
    key = String8(AUDIO_PARAMETER_KEY_HANDLE_FM);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore if device is 0
        if(device) {
            handleFm(device);
        }
        param.remove(key);
    }
#endif

    key = String8(ST_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableSlowTalk(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

    key = String8(VSID_KEY);
    if (param.getInt(key, (int &)mVSID) == NO_ERROR) {
        param.remove(key);
        key = String8(CALL_STATE_KEY);
        if (param.getInt(key, (int &)call_state) == NO_ERROR) {
            param.remove(key);
            mCallState = call_state;
            ALOGV("%s() vsid:%x, callstate:%x", __func__, mVSID, call_state);

            if(isAnyCallActive())
                doRouting(0);
        }
        param.remove(key);
    }
#ifdef QCOM_DS1_DOLBY_DDP
    key = String8("ddp_device");
    if (param.getInt(key, ddp_dev) == NO_ERROR) {
        param.remove(key);
    }
    key = String8("ddp_chancap");
    if (param.getInt(key, ddp_ch_cap) == NO_ERROR) {
        param.remove(key);
        parseDDPParams(ddp_dev, ddp_ch_cap, &param);
    }
#endif

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    key = String8(AUDIO_PARAMETER_KEY_MAD);
    if (param.get(key, value) == NO_ERROR) {
        if (mListenHw) {
            status = mListenHw->setParameters(keyValuePairs);
        }
        param.remove(key);
    }
#endif
    if (status != NO_ERROR || param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardwareALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key;
    int device;
#ifdef QCOM_FLUENCE_ENABLED
    key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        value = String8("false");
        param.add(key, value);
    }

    key = String8(AUDIO_PARAMETER_KEY_FLUENCE_TYPE);
    if (param.get(key, value) == NO_ERROR) {
    if ((mDevSettingsFlag & QMIC_FLAG) &&
                               (mDevSettingsFlag & ~DMIC_FLAG))
            value = String8("quadmic");
    else if ((mDevSettingsFlag & DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("dualmic");
    else if ((mDevSettingsFlag & ~DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("none");
        param.add(key, value);
    }
#endif

#ifdef QCOM_FM_ENABLED

    key = String8(AUDIO_PARAMETER_KEY_HANDLE_A2DP_DEVICE);
    if ( param.get(key,value) == NO_ERROR ) {
        param.add(key, String8("true"));
    }

    key = String8("Fm-radio");
    if ( param.get(key,value) == NO_ERROR ) {
        if ( mIsFmActive ) {
            param.addInt(String8("isFMON"), true );
        }
    }
#endif

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if(mBluetoothVGS)
           param.addInt(String8("isVGS"), true);
    }
#ifdef QCOM_SSR_ENABLED
    key = String8(AUDIO_PARAMETER_KEY_SSR);
    if (param.get(key, value) == NO_ERROR) {
        char ssr_enabled[6] = "false";
        property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
        if (!strncmp("true", ssr_enabled, 4)) {
            value = String8("true");
        }
        param.add(key, value);
    }
#endif

    key = String8("A2dpSuspended");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpDevice != NULL) {
            value = mA2dpDevice->get_parameters(mA2dpDevice,key);
        }
        param.add(key, value);
    }

    key = String8("a2dp_sink_address");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpStream != NULL) {
            value = mA2dpStream->common.get_parameters(&mA2dpStream->common,key);
        }
        param.add(key, value);
    }
    key = String8("tunneled-input-formats");
    if ( param.get(key,value) == NO_ERROR ) {
        int newMode = mode();
        if(newMode != AUDIO_MODE_IN_COMMUNICATION && newMode != AUDIO_MODE_IN_CALL){
            ALOGD("Add tunnel AWB to audio parameter");
            param.addInt(String8("AWB"), true );
        }
    }

    key = String8(AudioParameter::keyRouting);
    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, mCurDevice);
    }

#ifdef QCOM_PROXY_DEVICE_ENABLED
    key = String8(AUDIO_CAN_OPEN_PROXY);
    if(param.get(key, value) == NO_ERROR) {
        param.addInt(key, mCanOpenProxy);
    }
#endif

    key = String8("snd_card_name");
    if (param.get(key, value) == NO_ERROR) {
        struct snd_ctl_card_info *cardInfo;
        cardInfo = mALSADevice->getSoundCardInfo();
        param.add(key, String8((const char*)cardInfo->name));
    }

    key = String8("snd_card_index");
    if (param.get(key, value) == NO_ERROR) {
        struct snd_ctl_card_info *cardInfo;
        cardInfo = mALSADevice->getSoundCardInfo();
        param.addInt(key, cardInfo->card);
    }

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    if (mListenHw) {
        mListenHw->getParameters(keys);
    }
#endif

    ALOGV("AudioHardwareALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

#ifdef QCOM_USBAUDIO_ENABLED
void AudioHardwareALSA::closeUSBPlayback()
{
    ALOGD("closeUSBPlayback, musbPlaybackState: %d", musbPlaybackState);
    musbPlaybackState = 0;
    mAudioUsbALSA->exitPlaybackThread(SIGNAL_EVENT_KILLTHREAD);
}

void AudioHardwareALSA::closeUSBRecording()
{
    ALOGD("closeUSBRecording");
    musbRecordingState = 0;
    mAudioUsbALSA->exitRecordingThread(SIGNAL_EVENT_KILLTHREAD);
}

void AudioHardwareALSA::closeUsbPlaybackIfNothingActive(){
    ALOGD("closeUsbPlaybackIfNothingActive, musbPlaybackState: %d", musbPlaybackState);
    if(!musbPlaybackState && mAudioUsbALSA != NULL) {
        setProxyProperty(1);
        mAudioUsbALSA->exitPlaybackThread(SIGNAL_EVENT_KILLTHREAD);
    }
}

void AudioHardwareALSA::closeUsbRecordingIfNothingActive(){
    ALOGD("closeUsbRecordingIfNothingActive, musbRecordingState: %d", musbRecordingState);
    if(!musbRecordingState && mAudioUsbALSA != NULL) {
        ALOGD("Closing USB Recording Session as no stream is active");
        mAudioUsbALSA->exitRecordingThread(SIGNAL_EVENT_KILLTHREAD);
    }
}

void AudioHardwareALSA::startUsbPlaybackIfNotStarted(){
    ALOGD("Starting the USB playback %d kill %d", musbPlaybackState,
             mAudioUsbALSA->getkillUsbPlaybackThread());
    if((!musbPlaybackState) || (mAudioUsbALSA->getkillUsbPlaybackThread() == true)) {
           setProxyProperty(0);
           mAudioUsbALSA->startPlayback();
    }
}

void AudioHardwareALSA::startUsbRecordingIfNotStarted(){
    ALOGD("Starting the recording musbRecordingState: %d killUsbRecordingThread %d",
          musbRecordingState, mAudioUsbALSA->getkillUsbRecordingThread());
    if((!musbRecordingState) || (mAudioUsbALSA->getkillUsbRecordingThread() == true)) {
        mAudioUsbALSA->startRecording();
    }
}
#endif

#ifdef QCOM_WFD_ENABLED
void AudioHardwareALSA::getWFDAudioSinkCaps( int32_t &channelCount, int32_t &sampleRate) {
    // Sample rate set with 48K
    sampleRate = 48000;
    channelCount = mALSADevice->getWFDChannelCaps();
    if(0 == channelCount) {
        // Default is stereo with 2 channels
        channelCount = 2;
        ALOGW("ALSADevice gave invalid channel capabilities\
           channelCount = %d, sampleRate = %d", channelCount, sampleRate);
    }
}
#endif

status_t AudioHardwareALSA::doRouting(int device)
{
    Mutex::Autolock autoLock(mLock);
    int newMode = mode();
    bool isRouted = false;

    if(device)
        mALSADevice->mCurDevice = device;
    if ((device == AudioSystem::DEVICE_IN_VOICE_CALL)
#ifdef QCOM_FM_ENABLED
        || (device == AudioSystem::DEVICE_IN_FM_RX)
        || (device == AudioSystem::DEVICE_IN_FM_RX_A2DP)
#endif
        || (device == AudioSystem::DEVICE_IN_COMMUNICATION)
        ) {
        ALOGD("Ignoring routing for FM/INCALL/VOIP recording");
        return NO_ERROR;
    }
    ALOGD("device = 0x%x,mCurDevice 0x%x", device, mCurDevice);
    if (device == 0)
        device = mCurDevice;
#ifdef QCOM_DS1_DOLBY_DDP
    if (device != mCurDevice)
        setDDPEndpParams(device);
#endif

    ALOGV("doRouting: device %#x newMode %d mVoiceCallState %x \
           mVolteCallActive %x mVoice2CallActive %x mIsFmActive %x",
          device, newMode, mVoiceCallState,
          mVolteCallState, mVoice2CallState, mIsFmActive);

    isRouted = routeCall(device, newMode, mVSID);

    if(!isRouted) {
#ifdef QCOM_USBAUDIO_ENABLED
        if(!(device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET) &&
            !(device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET) &&
            !(device & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET) &&
             (musbPlaybackState || musbRecordingState)){
                // mExtOutStream should be initialized before calling route
                // when switching form USB headset to ExtOut device
                if( mRouteAudioToExtOut == true ) {
                    switchExtOut(device);
                }
                ALSAHandleList::iterator it = mDeviceList.end();
                it--;
                mALSADevice->route(&(*it), (uint32_t)device, newMode);
                ALOGD("USB UNPLUGGED, setting musbPlaybackState to 0");
                closeUSBRecording();
                closeUSBPlayback();
        } else if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
                  (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
                    ALOGD("Routing everything to prox now");
                    // stopPlaybackOnExtOut should be called to close
                    // ExtOutthread when switching from ExtOut device to USB headset
                    if( mRouteAudioToExtOut == true ) {
                        uint32_t activeUsecase = getExtOutActiveUseCases_l();
                        status_t err = stopPlaybackOnExtOut_l(activeUsecase);
                        if(err) {
                            ALOGW("stopPlaybackOnExtOut_l failed = %d", err);
                            return err;
                        }
                    }
                    ALSAHandleList::iterator it = mDeviceList.end();
                    it--;
                    if (device != mCurDevice) {
                        if(musbPlaybackState)
                            closeUSBPlayback();
                    }
                    mALSADevice->route(&(*it), device, newMode);
                    for(it = mDeviceList.begin(); it != mDeviceList.end(); ++it) {
                         if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
                            (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
                                 ALOGD("doRouting: LPA device switch to proxy");
                                 startUsbPlaybackIfNotStarted();
                                 musbPlaybackState |= USBPLAYBACKBIT_LPA;
                                 break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
                                    ALOGD("doRouting: Tunnel Player device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_TUNNEL;
                                    break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2))) {
                                    ALOGD("doRouting: Tunnel Player device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_TUNNEL2;
                                    break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL3)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3))) {
                                    ALOGD("doRouting: Tunnel Player device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_TUNNEL3;
                                    break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL4)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL4))) {
                                    ALOGD("doRouting: Tunnel Player device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_TUNNEL4;
                                    break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
                                    ALOGD("doRouting: VOICE device switch to proxy");
                                    startUsbRecordingIfNotStarted();
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_VOICECALL;
                                    musbRecordingState |= USBPLAYBACKBIT_VOICECALL;
                                    break;
                        }else if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
                                 (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                                    ALOGD("doRouting: FM device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_FM;
                                    break;
                         }
                    }
        } else
#endif
        if ((isExtOutDevice(device)) && mRouteAudioToExtOut == true)  {
            ALOGD(" External Output Enabled - Routing everything to proxy now");
            switchExtOut(device);
            ALSAHandleList::iterator it = mDeviceList.end();
            it--;
            status_t err = NO_ERROR;
            uint32_t activeUsecase = useCaseStringToEnum(it->useCase);
            if (!((device & AudioSystem::DEVICE_OUT_ALL_A2DP) &&
                  (mCurRxDevice & AUDIO_DEVICE_OUT_ALL_USB))) {
                if ((activeUsecase == USECASE_HIFI_LOW_POWER) ||
                    (activeUsecase == USECASE_HIFI_TUNNEL) ||
                    (activeUsecase == USECASE_HIFI_TUNNEL2) ||
                    (activeUsecase == USECASE_HIFI_TUNNEL3) ||
                    (activeUsecase == USECASE_HIFI_TUNNEL4)) {
                    if (device != mCurRxDevice) {
                        if((isExtOutDevice(mCurRxDevice)) &&
                           (isExtOutDevice(device))) {
                            activeUsecase = getExtOutActiveUseCases_l();
                            stopPlaybackOnExtOut_l(activeUsecase);
                            mRouteAudioToExtOut = true;
                        }
                        mALSADevice->route(&(*it),(uint32_t)device, newMode);
                    }
                    err = startPlaybackOnExtOut_l(activeUsecase);
                } else {
                    //WHY NO check for prev device here?
                    if (device != mCurRxDevice) {
                        if((isExtOutDevice(mCurRxDevice)) &&
                            (isExtOutDevice(device))) {
                            activeUsecase = getExtOutActiveUseCases_l();
                            stopPlaybackOnExtOut_l(activeUsecase);
                            mALSADevice->route(&(*it),(uint32_t)device, newMode);
                            mRouteAudioToExtOut = true;
                            startPlaybackOnExtOut_l(activeUsecase);
                        } else {
                           mALSADevice->route(&(*it),(uint32_t)device, newMode);
                        }
                    }
                    if (activeUsecase == USECASE_FM){
                        err = startPlaybackOnExtOut_l(activeUsecase);
                    }
                }
                if(err) {
                    ALOGW("startPlaybackOnExtOut_l for hardware output failed err = %d", err);
                    stopPlaybackOnExtOut_l(activeUsecase);
                    mALSADevice->route(&(*it),(uint32_t)mCurRxDevice, newMode);
                    return err;
                }
            }
        } else if((device & AudioSystem::DEVICE_OUT_ALL) &&
                  (!isExtOutDevice(device)) &&
                   mRouteAudioToExtOut == true ) {
            ALOGD(" ExtOut Disable on hardware output");
            ALSAHandleList::iterator it = mDeviceList.end();
            it--;
            status_t err;
            uint32_t activeUsecase = getExtOutActiveUseCases_l();
            err = stopPlaybackOnExtOut_l(activeUsecase);
            if(err) {
                ALOGW("stopPlaybackOnExtOut_l failed = %d", err);
                return err;
            }
            if (device != mCurDevice) {
                mALSADevice->route(&(*it),(uint32_t)device, newMode);
            }
        } else {
             setInChannels(device);
             ALSAHandleList::iterator it = mDeviceList.end();
             it--;
             mALSADevice->route(&(*it), (uint32_t)device, newMode);
        }
    }
    mCurDevice = device;
    if (device & AudioSystem::DEVICE_OUT_ALL) {
        mCurRxDevice = device;
    }
    return NO_ERROR;
}

void AudioHardwareALSA::setInChannels(int device)
{
     ALSAHandleList::iterator it;

     if (device & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
         for(it = mDeviceList.begin(); it != mDeviceList.end(); ++it) {
             if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC,
                 strlen(SND_USE_CASE_VERB_HIFI_REC)) ||
                 !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC,
                 strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
                 mALSADevice->setInChannels(it->channels);
                 return;
             }
         }
     }

     mALSADevice->setInChannels(1);
}

uint32_t AudioHardwareALSA::getVoipMode(int format)
{
    switch(format) {
    case AUDIO_FORMAT_PCM_16_BIT:
               return MODE_PCM;
         break;
    case AUDIO_FORMAT_AMR_NB:
               return MODE_AMR;
         break;
    case AUDIO_FORMAT_AMR_WB:
               return MODE_AMR_WB;
         break;

#ifdef QCOM_AUDIO_FORMAT_ENABLED
    case AUDIO_FORMAT_EVRC:
               return MODE_IS127;
         break;

    case AUDIO_FORMAT_EVRCB:
               return MODE_4GV_NB;
         break;
    case AUDIO_FORMAT_EVRCWB:
               return MODE_4GV_WB;
         break;
#endif
    default:
               return MODE_PCM;
    }
}

#ifdef QCOM_LISTEN_FEATURE_ENABLE
status_t AudioHardwareALSA::openListenSession(ListenSession** handle)
{
    status_t status = NO_INIT;
    ALOGV("openListenSession: Enter");
    if (mListenHw)
        status = mListenHw->openListenSession(handle);
    ALOGD("openListenSession: Exit status=%d", status);
    return status;
}

status_t AudioHardwareALSA::closeListenSession(ListenSession* handle)
{
    status_t status = NO_INIT;
    ALOGV("closeListenSession: Enter");
    if (mListenHw)
        status = mListenHw->closeListenSession(handle);
    ALOGV("closeListenSession: Exit status=%d", status);
    return status;
}

status_t AudioHardwareALSA::setMadObserver(listen_callback_t cb_func)
{
    status_t status = NO_INIT;
    ALOGV("setMadObserver: Enter");
    if (mListenHw)
        status = mListenHw->setMadObserver(cb_func);
    ALOGV("setMadObserver: Exit status=%d", status);
    return status;
}
#endif //QCOM_LISTEN_FEATURE_ENABLE

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    Mutex::Autolock autoLock(mLock);

    audio_output_flags_t flags = static_cast<audio_output_flags_t> (*status);

    ALOGD("openOutputStream: devices 0x%x channels %d sampleRate %d flags %x",
         devices, *channels, *sampleRate, flags);

    status_t err = BAD_VALUE;
#ifdef QCOM_OUTPUT_FLAGS_ENABLED
    if (flags & (AUDIO_OUTPUT_FLAG_LPA | AUDIO_OUTPUT_FLAG_TUNNEL)) {
        int type = !(flags & AUDIO_OUTPUT_FLAG_LPA); //0 for LPA, 1 for tunnel
        AudioSessionOutALSA *out = new AudioSessionOutALSA(this, devices, *format, *channels,
                                                           *sampleRate, type, &err);
        if(err != NO_ERROR) {
            mLock.unlock();
            delete out;
            out = NULL;
            mLock.lock();
        }
        if (status) *status = err;
        return out;
    }
#endif
    AudioStreamOutALSA *out = 0;
    ALSAHandleList::iterator it;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        ALOGE("openOutputStream called with bad devices");
        return out;
    }

    if(isExtOutDevice(devices)) {
        ALOGV("Set Capture from proxy true");
        mRouteAudioToExtOut = true;
    }

#ifdef QCOM_OUTPUT_FLAGS_ENABLED
    if((flags & AUDIO_OUTPUT_FLAG_DIRECT) && (flags & AUDIO_OUTPUT_FLAG_VOIP_RX)&&
       ((*sampleRate == VOIP_SAMPLING_RATE_8K) || (*sampleRate == VOIP_SAMPLING_RATE_16K))) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    ALOGD("openOutput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
      if(voipstream_active == false) {
         mVoipOutStreamCount = 0;
         mVoipMicMute = false;
         alsa_handle_t alsa_handle;
         unsigned long bufferSize;
         if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
             bufferSize = VOIP_BUFFER_SIZE_8K;
         }
         else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
             bufferSize = VOIP_BUFFER_SIZE_16K;
         }
         else {
             ALOGE("unsupported samplerate %d for voip",*sampleRate);
             if (status) *status = err;
                 return out;
          }
          alsa_handle.module = mALSADevice;
          alsa_handle.bufferSize = bufferSize;
          alsa_handle.devices = devices;
          alsa_handle.handle = 0;
          if(*format == AUDIO_FORMAT_PCM_16_BIT)
              alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          else
              alsa_handle.format = *format;
          alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
          alsa_handle.sampleRate = *sampleRate;
          alsa_handle.latency = VOIP_PLAYBACK_LATENCY;
          alsa_handle.rxHandle = 0;
          alsa_handle.ucMgr = mUcMgr;
          mALSADevice->setVoipConfig(getVoipMode(*format), mVoipBitRate);
          char *use_case;
          snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
          } else {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
          }
          free(use_case);
          mDeviceList.push_back(alsa_handle);
          it = mDeviceList.end();
          it--;
          ALOGD("openoutput: mALSADevice->route useCase %s mCurDevice %d mVoipOutStreamCount %d mode %d",
                it->useCase,mCurDevice, mVoipOutStreamCount, mode());
          if((mCurDevice & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
             (mCurDevice & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)||
             (mCurDevice & AudioSystem::DEVICE_OUT_PROXY)){
              ALOGD("Routing to proxy for normal voip call in openOutputStream");
              mALSADevice->route(&(*it), mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
#ifdef QCOM_USBAUDIO_ENABLED
              ALOGD("enabling VOIP in openoutputstream, musbPlaybackState: %d", musbPlaybackState);
              startUsbPlaybackIfNotStarted();
              musbPlaybackState |= USBPLAYBACKBIT_VOIPCALL;
              ALOGD("Starting recording in openoutputstream, musbRecordingState: %d", musbRecordingState);
              startUsbRecordingIfNotStarted();
              musbRecordingState |= USBRECBIT_VOIPCALL;
#endif
           } else{
              mALSADevice->route(&(*it), mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
          }
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
              snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
          } else {
              snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
          }
      #ifdef QCOM_LISTEN_FEATURE_ENABLE
          //Notify to listen HAL that capture is active
          if (mListenHw) {
              mListenHw->notifyEvent(AUDIO_CAPTURE_ACTIVE);
          }
      #endif

          err = mALSADevice->startVoipCall(&(*it));
          if (err) {
              ALOGE("Device open failed");
        #ifdef QCOM_LISTEN_FEATURE_ENABLE
              //Notify to listen HAL that voip call is inactive
            if (mListenHw) {
                  mListenHw->notifyEvent(AUDIO_CAPTURE_INACTIVE);
            }
        #endif
              return NULL;
          }
      }
      out = new AudioStreamOutALSA(this, &(*it));
      err = out->set(format, channels, sampleRate, devices);
      if(err == NO_ERROR) {
          mVoipOutStreamCount++;   //increment VoipOutstreamCount only if success
          ALOGD("openoutput mVoipOutStreamCount %d",mVoipOutStreamCount);
      } else {
          mLock.unlock();
          delete out;
          out = NULL;
          ALOGE("AudioStreamOutALSA->set() failed, return NULL");

          mLock.lock();
      }
      if (status) *status = err;
      return out;
    } else
#endif
    if ((flags & AUDIO_OUTPUT_FLAG_DIRECT) &&
        ((devices == AUDIO_DEVICE_OUT_AUX_DIGITAL)
#ifdef QCOM_WFD_ENABLED
        || (devices == AudioSystem::DEVICE_OUT_PROXY)
#endif
        )) {
        ALOGD("Multi channel PCM");
        alsa_handle_t alsa_handle;
        EDID_AUDIO_INFO info = { 0 };

        alsa_handle.module = mALSADevice;
        alsa_handle.devices = devices;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;

        if(devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
#ifdef TARGET_B_FAMILY
        char hdmiEDIDData[MAX_SHORT_AUDIO_DESC_CNT + 1];
                              // additional 1 byte for length of the EDID
        if ((devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) &&
                 mALSADevice->getEDIDData(hdmiEDIDData) == NO_ERROR) {
            if (!AudioUtil::getHDMIAudioSinkCaps(&info, hdmiEDIDData)) {
                ALOGE("openOutputStream: Failed to get HDMI sink capabilities");
                return NULL;
            }
        }
#else
        if (!AudioUtil::getHDMIAudioSinkCaps(&info)) {
            ALOGE("openOutputStream: Failed to get HDMI sink capabilities");
            return NULL;
        }
#endif
        }
        if (0 == *channels) {
            if(devices == AUDIO_DEVICE_OUT_AUX_DIGITAL)
                alsa_handle.channels = info.AudioBlocksArray[info.nAudioBlocks-1].nChannels;
#ifdef QCOM_WFD_ENABLED
            else if(devices == AudioSystem::DEVICE_OUT_PROXY) {
                ALOGD("Setting Sink capability for WFD Direct Output");
                int32_t sampleRate = 0 , channelCount = 0;
                //We need to know if sink supports 6 or 8 channel.
                //For stereo we dont need to set channel count as
                //we get stereo data from proxy by default.
                getWFDAudioSinkCaps(channelCount, sampleRate);
                mALSADevice->setProxyPortChannelCount(channelCount);
            }
#endif
            if (alsa_handle.channels > 8) {
                alsa_handle.channels = 8;
            }
            *channels = audio_channel_out_mask_from_count(alsa_handle.channels);
        } else {
            alsa_handle.channels = AudioSystem::popCount(*channels);
        }
        if (alsa_handle.channels > 2) {
            alsa_handle.bufferSize = DEFAULT_MULTI_CHANNEL_BUF_SIZE;
        } else {
            alsa_handle.bufferSize = DEFAULT_BUFFER_SIZE;
        }
        if (0 == *sampleRate) {
            if(devices == AUDIO_DEVICE_OUT_AUX_DIGITAL){
                alsa_handle.sampleRate = info.AudioBlocksArray[info.nAudioBlocks-1].nSamplingFreq;
                *sampleRate = alsa_handle.sampleRate;
            }
#ifdef QCOM_PROXY_DEVICE_ENABLED
            else if (devices & AudioSystem::DEVICE_OUT_PROXY) {
                *sampleRate = 48000;
            }
#endif
        } else {
            alsa_handle.sampleRate = *sampleRate;
        }
        alsa_handle.latency = PLAYBACK_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        ALOGD("alsa_handle.channels %d alsa_handle.sampleRate %d",alsa_handle.channels,alsa_handle.sampleRate);

        char *use_case;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI2 , sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC2, sizeof(alsa_handle.useCase));
        }
        free(use_case);
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        out = new AudioStreamOutALSA(this, &(*it));
        err = out->set(format, channels, sampleRate, devices);
        if (status) *status = err;
        return out;
    } else {
      alsa_handle_t alsa_handle;
      unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

      for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
          bufferSize &= ~b;

      alsa_handle.module = mALSADevice;
      alsa_handle.bufferSize = bufferSize;
      alsa_handle.devices = devices;
      alsa_handle.handle = 0;
      alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
      alsa_handle.channels = DEFAULT_CHANNEL_MODE;
      alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
      alsa_handle.latency = PLAYBACK_LATENCY;
      alsa_handle.rxHandle = 0;
      alsa_handle.ucMgr = mUcMgr;
#ifdef QCOM_TUNNEL_LPA_ENABLED
      alsa_handle.session = NULL;
#endif
      alsa_handle.isFastOutput = false;

      char *use_case;
      snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);

#ifdef QCOM_OUTPUT_FLAGS_ENABLED
      if (flags & AUDIO_OUTPUT_FLAG_FAST) {
          alsa_handle.bufferSize = PLAYBACK_LOW_LATENCY_BUFFER_SIZE;
          alsa_handle.latency = PLAYBACK_LOW_LATENCY;
          alsa_handle.isFastOutput = true;
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
          } else {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
          }
      } else
#endif
      {
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI, sizeof(alsa_handle.useCase));
          } else {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(alsa_handle.useCase));
          }
      }
      free(use_case);
      mDeviceList.push_back(alsa_handle);
      ALSAHandleList::iterator it = mDeviceList.end();
      it--;
      ALOGD("useCase %s", it->useCase);
      mALSADevice->route(&(*it), devices, mode());
#ifdef QCOM_OUTPUT_FLAGS_ENABLED
      if (flags & AUDIO_OUTPUT_FLAG_FAST) {
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC)) {
             snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC);
          } else {
             snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC);
          }
      } else
#endif
      {
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
             snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI);
          } else {
             snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
          }
      }
      err = mALSADevice->open(&(*it));
      if (err) {
          ALOGE("Device open failed");
      } else {
          out = new AudioStreamOutALSA(this, &(*it));
          err = out->set(format, channels, sampleRate, devices);
      }

      if (status) *status = err;
      return out;
    }
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

#ifdef QCOM_TUNNEL_LPA_ENABLED
AudioStreamOut *
AudioHardwareALSA::openOutputSession(uint32_t devices,
                                     int *format,
                                     status_t *status,
                                     int sessionId,
                                     uint32_t samplingRate,
                                     uint32_t channels)
{
    Mutex::Autolock autoLock(mLock);
    ALOGD("openOutputSession = %d" ,sessionId);
    AudioStreamOutALSA *out = 0;
    status_t err = BAD_VALUE;

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    char *use_case;
    if(sessionId == TUNNEL_SESSION_ID) {
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_TUNNEL, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_TUNNEL, sizeof(alsa_handle.useCase));
        }
    } else {
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LPA, sizeof(alsa_handle.useCase));
        }
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    ALOGD("useCase %s", it->useCase);
#ifdef QCOM_WFD_ENABLED
    if(devices & AudioSystem::DEVICE_OUT_PROXY){
        ALOGE("Setting Sink capability for WFD");
        int32_t sampleRate = 0 , channelCount = 0;
        //We need to know if sink supports 6 or 8 channel.
        //For stereo we dont need to set channel count as
        //we get stereo data from proxy by default.
        getWFDAudioSinkCaps(channelCount, sampleRate);
        mALSADevice->setProxyPortChannelCount(channelCount);
    }
#endif

#ifdef QCOM_USBAUDIO_ENABLED
    if((devices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
       (devices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
        ALOGE("Routing to proxy for LPA in openOutputSession");
        mALSADevice->route(&(*it), devices, mode());
        devices = AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET;
        ALOGD("Starting USBPlayback for LPA");
        startUsbPlaybackIfNotStarted();
        musbPlaybackState |= USBPLAYBACKBIT_LPA;
    } else
#endif
    {
        mALSADevice->route(&(*it), devices, mode());
    }
    if(sessionId == TUNNEL_SESSION_ID) {
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_TUNNEL);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
        }
    }
    else {
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOW_POWER);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LPA);
        }
    }
    err = mALSADevice->open(&(*it));
    out = new AudioStreamOutALSA(this, &(*it));

    if (status) *status = err;
       return out;
}

void
AudioHardwareALSA::closeOutputSession(AudioStreamOut* out)
{
    delete out;
}
#endif

AudioStreamIn *
AudioHardwareALSA::openInputStream(uint32_t devices,
                                   int *format,
                                   uint32_t *channels,
                                   uint32_t *sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    Mutex::Autolock autoLock(mLock);
    char *use_case;
    int newMode = mode();
    uint32_t route_devices;

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;
    ALSAHandleList::iterator it;

    ALOGD("openInputStream: devices 0x%x format 0x%x channels %d sampleRate %d", devices, *format, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        ALOGE("openInputStream failed error:0x%x devices:%x",err,devices);
        return in;
    }
#ifdef QCOM_LISTEN_FEATURE_ENABLE
    //Notify to listen HAL that Audio capture is active
    if (mListenHw) {
        mListenHw->notifyEvent(AUDIO_CAPTURE_ACTIVE);
    }
#endif

    if((devices == AudioSystem::DEVICE_IN_COMMUNICATION) && (mVoipInStreamCount < 1) &&
       ((*sampleRate == VOIP_SAMPLING_RATE_8K) || (*sampleRate == VOIP_SAMPLING_RATE_16K))) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    ALOGD("openInput:  it->rxHandle %p it->handle %p",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
        if(voipstream_active == false) {
           mVoipInStreamCount = 0;
           mVoipMicMute = false;
           alsa_handle_t alsa_handle;
           unsigned long bufferSize;
           if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
               bufferSize = VOIP_BUFFER_SIZE_8K;
           }
           else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
               bufferSize = VOIP_BUFFER_SIZE_16K;
           }
           else {
               ALOGE("unsupported samplerate %d for voip",*sampleRate);
               if (status) *status = err;
               return in;
           }
           alsa_handle.module = mALSADevice;
           alsa_handle.bufferSize = bufferSize;
           alsa_handle.devices = devices;
           alsa_handle.handle = 0;
          if(*format == AUDIO_FORMAT_PCM_16_BIT)
              alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          else
              alsa_handle.format = *format;
           alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
           alsa_handle.sampleRate = *sampleRate;
           alsa_handle.latency = VOIP_RECORD_LATENCY;
           alsa_handle.rxHandle = 0;
           alsa_handle.ucMgr = mUcMgr;
          mALSADevice->setVoipConfig(getVoipMode(*format), mVoipBitRate);
           snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
           if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
           } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
           }
           free(use_case);
           mDeviceList.push_back(alsa_handle);
           it = mDeviceList.end();
           it--;
           ALOGD("mCurrDevice: %d", mCurDevice);

#ifdef QCOM_USBAUDIO_ENABLED
           if((mCurDevice == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
              (mCurDevice == AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
              ALOGD("Routing everything from proxy for voipcall");
              mALSADevice->route(&(*it), AudioSystem::DEVICE_IN_PROXY, AUDIO_MODE_IN_COMMUNICATION);
              ALOGD("enabling VOIP in openInputstream, musbPlaybackState: %d", musbPlaybackState);
              startUsbPlaybackIfNotStarted();
              musbPlaybackState |= USBPLAYBACKBIT_VOIPCALL;
              ALOGD("Starting recording in openoutputstream, musbRecordingState: %d", musbRecordingState);
              startUsbRecordingIfNotStarted();
              musbRecordingState |= USBRECBIT_VOIPCALL;
           } else
#endif
           {
               mALSADevice->route(&(*it),mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
           }
           if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
               snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
           } else {
               snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
           }
           if(sampleRate) {
               it->sampleRate = *sampleRate;
           }
           if(channels) {
               it->channels = AudioSystem::popCount(*channels);
               setInChannels(devices);
           }
           err = mALSADevice->startVoipCall(&(*it));
           if (err) {
               ALOGE("Error opening pcm input device");
        #ifdef QCOM_LISTEN_FEATURE_ENABLE
               //Notify to listen HAL that Audio capture is inactive
            if (mListenHw) {
                   mListenHw->notifyEvent(AUDIO_CAPTURE_INACTIVE);
            }
        #endif
               return NULL;
           }
        }
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate, devices);
        if(err == NO_ERROR) {
            mVoipInStreamCount++;   //increment VoipInstreamCount only if success
            ALOGD("OpenInput mVoipInStreamCount %d",mVoipInStreamCount);
        } else {
            mLock.unlock();
            delete in;
            in = NULL;
            ALOGE("AudioStreamInALSA->set() failed, return NULL");

            mLock.lock();
        }
        ALOGD("openInput: After Get alsahandle");
        if (status) *status = err;
        return in;
      } else {
        alsa_handle_t alsa_handle;
        unsigned long bufferSize = MIN_CAPTURE_BUFFER_SIZE_PER_CH;

        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = devices;
        alsa_handle.handle = 0;
        if(*format == AUDIO_FORMAT_PCM_16_BIT)
            alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        else
            alsa_handle.format = *format;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = android::AudioRecord::DEFAULT_SAMPLE_RATE;
        alsa_handle.latency = RECORD_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AUDIO_MODE_IN_CALL)) {
                ALOGD("openInputStream: into incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK)) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_COMPRESSED_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_MONO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_COMPRESSED_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) {
                   if (mFusion3Platform == false) {
                       strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL,
                               sizeof(SND_USE_CASE_MOD_CAPTURE_VOICE_UL));
                   } else {
                       /* Use normal audio recording for Fusion3 target, this behavior
                          will be changed in Fusion4
                        */
                       strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC,
                               sizeof(SND_USE_CASE_MOD_CAPTURE_MUSIC));
                   }
               }
#ifdef QCOM_FM_ENABLED
            } else if((devices == AudioSystem::DEVICE_IN_FM_RX)) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(alsa_handle.useCase));
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, sizeof(alsa_handle.useCase));
#endif
            } else {
                char value[128];
                property_get("persist.audio.lowlatency.rec",value,"0");
                if (!strcmp("true", value)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
                } else if (*format == AUDIO_FORMAT_AMR_WB) {
                    ALOGV("Format AMR_WB, open compressed capture");
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED, sizeof(alsa_handle.useCase));
                } else {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(alsa_handle.useCase));
                }
            }
        } else {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AUDIO_MODE_IN_CALL)) {
                ALOGD("openInputStream: incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK)) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_INCALL_REC,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_UL_DL_REC,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_MONO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_INCALL_REC,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DL_REC,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) {
                   if (mFusion3Platform == false) {
                       strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_UL_REC,
                               sizeof(SND_USE_CASE_VERB_UL_REC));
                   } else {
                       strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC,
                               sizeof(SND_USE_CASE_VERB_HIFI_REC));
                   }
                }
#ifdef QCOM_FM_ENABLED
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_REC, sizeof(alsa_handle.useCase));
            } else if (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_A2DP_REC, sizeof(alsa_handle.useCase));
#endif
            } else {
                char value[128];
                property_get("persist.audio.lowlatency.rec",value,"0");
                if (!strcmp("true", value)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_REC, sizeof(alsa_handle.useCase));
                } else if (*format == AUDIO_FORMAT_AMR_WB) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED, sizeof(alsa_handle.useCase));
                } else {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(alsa_handle.useCase));
                }
            }
        }
        free(use_case);
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        //update channel info before do routing
        if(channels) {
            it->channels = AudioSystem::popCount((*channels) &
                      (AUDIO_CHANNEL_IN_STEREO
                       | AUDIO_CHANNEL_IN_MONO
#ifdef QCOM_SSR_ENABLED
                       | AUDIO_CHANNEL_IN_5POINT1
#endif
                       ));
            ALOGV("updated channel info: channels=%d", it->channels);
            setInChannels(devices);
        }
        if (devices == AudioSystem::DEVICE_IN_VOICE_CALL){
           /* Add current devices info to devices to do route */
#ifdef QCOM_USBAUDIO_ENABLED
            if(mCurDevice == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET ||
               mCurDevice == AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET){
                ALOGD("Routing everything from proxy for VOIP call");
                route_devices = devices | AudioSystem::DEVICE_IN_PROXY;
            } else
#endif
            {
            route_devices = devices | mCurDevice;
            }
            mALSADevice->route(&(*it), route_devices, mode());
        } else {
#ifdef QCOM_USBAUDIO_ENABLED
            if(devices & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET ||
               devices & AudioSystem::DEVICE_IN_PROXY) {
                devices |= AudioSystem::DEVICE_IN_PROXY;
                ALOGD("routing everything from proxy");
            mALSADevice->route(&(*it), devices, mode());
            } else
#endif
            {
                mALSADevice->route(&(*it), devices, mode());
            }
        }

        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED) ||
#ifdef QCOM_FM_ENABLED
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_A2DP_REC) ||
#endif
           !strcmp(it->useCase, SND_USE_CASE_VERB_DL_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_UL_DL_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_DL) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_UL_DL) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_INCALL_REC)) {
            snd_use_case_set(mUcMgr, "_verb", it->useCase);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", it->useCase);
        }
        if(sampleRate) {
            it->sampleRate = *sampleRate;
        }
        if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            ALOGD("OpenInputStream: getInputBufferSize sampleRate:%d format:%d, channels:%d", it->sampleRate,*format,it->channels);
            it->bufferSize = getInputBufferSize(it->sampleRate,*format,it->channels);
        }

#ifdef QCOM_SSR_ENABLED
        if (6 == it->channels) {
            if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
                || !strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED, strlen(SND_USE_CASE_VERB_HIFI_REC_COMPRESSED))
                || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))
                || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED))) {
                //Check if SSR is supported by reading system property
                char ssr_enabled[6] = "false";
                property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
                if (strncmp("true", ssr_enabled, 4)) {
                    if (status) *status = err;
                    ALOGE("openInputStream: FAILED:%d. Surround sound recording is not supported",*status);
                }
            }
        }
#endif
        err = mALSADevice->open(&(*it));
        if (err) {
           mDeviceList.erase(it);
           ALOGE("Error opening pcm input device");
        #ifdef QCOM_LISTEN_FEATURE_ENABLE
            //Notify to listen HAL that Audio capture is inactive
            if (mListenHw) {
                mListenHw->notifyEvent(AUDIO_CAPTURE_INACTIVE);
            }
        #endif

        } else {
           in = new AudioStreamInALSA(this, &(*it), acoustics);
           err = in->set(format, channels, sampleRate, devices);
        }
        if (status) *status = err;
        return in;
      }
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    #ifdef QCOM_LISTEN_FEATURE_ENABLE
        //Notify to listen HAL that Audio capture is inactive
        if (mListenHw) {
            mListenHw->notifyEvent(AUDIO_CAPTURE_INACTIVE);
        }
    #endif
    delete in;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    int newMode = mode();
    ALOGD("setMicMute  newMode %d state:%d",newMode,state);
    if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
        if (mVoipMicMute != state) {
             mVoipMicMute = state;
            ALOGD("setMicMute: mVoipMicMute %d", mVoipMicMute);
            if(mALSADevice) {
                mALSADevice->setVoipMicMute(state);
            }
        }
    } else {
        if (mMicMute != state) {
              mMicMute = state;
              ALOGD("setMicMute: mMicMute %d", mMicMute);
              if(mALSADevice) {
                 if(mVoiceCallState == CALL_ACTIVE ||
                    mVoiceCallState == CALL_LOCAL_HOLD)
                     mALSADevice->setMicMute(state);

                 if(mVoice2CallState == CALL_ACTIVE ||
                    mVoice2CallState == CALL_LOCAL_HOLD)
                     mALSADevice->setVoice2MicMute(state);

                 if(mVolteCallState == CALL_ACTIVE ||
                    mVolteCallState == CALL_LOCAL_HOLD)
                     mALSADevice->setVoLTEMicMute(state);
              }
        }
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    int newMode = mode();
    if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
        *state = mVoipMicMute;
    } else {
        *state = mMicMute;
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    size_t bufferSize = 0;
    if (format == AUDIO_FORMAT_PCM_16_BIT) {
        if((sampleRate == 8000 || sampleRate == 16000 || sampleRate == 32000)) {
#ifdef TARGET_B_FAMILY
            bufferSize = DEFAULT_IN_BUFFER_SIZE;
#else
            bufferSize = (sampleRate * channelCount * 20 * sizeof(int16_t)) / 1000;
#endif
        } else if (sampleRate == 11025 || sampleRate == 12000) {
            bufferSize = 256 * sizeof(int16_t)  * channelCount;
        } else if (sampleRate == 22050 || sampleRate == 24000) {
            bufferSize = 512 * sizeof(int16_t)  * channelCount;
        } else if (sampleRate == 44100 || sampleRate == 48000) {
            bufferSize = 1024 * sizeof(int16_t) * channelCount;
        }
        ALOGD("getInputBufferSize PCM 16 bit = %d", bufferSize);
    } else if ( /*Used for tunnel voip encoders.
                 * Not used for tunnel amr-wb encoding
                 * as it works on 1 frame worth 61 bytes
                 */
        format == AUDIO_FORMAT_AMR_NB
        || format == AUDIO_FORMAT_AMR_WB
#ifdef QCOM_AUDIO_FORMAT_ENABLED
        || format == AUDIO_FORMAT_EVRC
        || format == AUDIO_FORMAT_EVRCB
        || format == AUDIO_FORMAT_EVRCWB
#endif
    ) {
        bufferSize = (sampleRate * channelCount * 20 * sizeof(int16_t)) / 1000;
        ALOGD("getInputBufferSize AMRWB/AMRNB/EVRC = %d", bufferSize);
    } else {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * channelCount;
        ALOGE("getInputBufferSize bad format: %x use default input buffersize:%d", format, bufferSize);
    }
    ALOGD("getInputBufferSize returns(%d)for:sampleRate(%d)+format(%d)+channelCount(%d)",bufferSize,sampleRate,format,channelCount);
    return bufferSize;
}

#ifdef QCOM_FM_ENABLED
void AudioHardwareALSA::handleFm(int device)
{
    int newMode = mode();
    uint32_t activeUsecase = USECASE_NONE;

    if(device & AUDIO_DEVICE_OUT_FM && mIsFmActive == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        ALOGD("Start FM");
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsFmActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_FM);
        }
        mALSADevice->startFm(&(*it));
        activeUsecase = useCaseStringToEnum(it->useCase);
#ifdef QCOM_USBAUDIO_ENABLED
        if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
           (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
            ALOGD("Starting FM, musbPlaybackState %d", musbPlaybackState);
            startUsbPlaybackIfNotStarted();
            musbPlaybackState |= USBPLAYBACKBIT_FM;
        }
#endif
        if(isExtOutDevice(device)) {
            status_t err = NO_ERROR;
            mRouteAudioToExtOut = true;
            err = startPlaybackOnExtOut_l(activeUsecase);
            if(err) {
                ALOGE("startPlaybackOnExtOut_l for hardware output failed err = %d", err);
                stopPlaybackOnExtOut_l(activeUsecase);
            }
        }

    } else if (!(device & AUDIO_DEVICE_OUT_FM) && mIsFmActive == 1) {
        //i Stop FM Radio
        ALOGD("Stop FM");
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
              (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                mALSADevice->close(&(*it));
                activeUsecase = useCaseStringToEnum(it->useCase);
                //mALSADevice->route(&(*it), (uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsFmActive = 0;
#ifdef QCOM_USBAUDIO_ENABLED
        musbPlaybackState &= ~USBPLAYBACKBIT_FM;
        if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
           (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
            closeUsbPlaybackIfNothingActive();
        }
#endif
        if(mRouteAudioToExtOut == true) {
            status_t err = NO_ERROR;
            err = stopPlaybackOnExtOut_l(activeUsecase);
            if(err)
                ALOGE("stopPlaybackOnExtOut_l for hardware output failed err = %d", err);
        }

    }
}
#endif

void AudioHardwareALSA::disableVoiceCall(int mode, int device, uint32_t vsid)
{
    char *verb = getUcmVerbForVSID(vsid);
    char *modifier = getUcmModForVSID(vsid);

    if (verb == NULL || modifier == NULL) {
        ALOGE("%s(): Error, verb=%p or modifier=%p is NULL",
              __func__, verb, modifier);

            return;
    }

    for(ALSAHandleList::iterator it = mDeviceList.begin();
         it != mDeviceList.end(); ++it) {
        if((!strncmp(it->useCase, verb,
                     MAX(strlen(verb), strlen(it->useCase)))) ||
           (!strncmp(it->useCase, modifier,
                     MAX(strlen(modifier), strlen(it->useCase))))) {
            ALOGV("Disabling voice call vsid:%x", vsid);
            mALSADevice->close(&(*it), vsid);
            mALSADevice->route(&(*it), (uint32_t)device, mode);
            mDeviceList.erase(it);
            break;
        }
    }

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    //Notify to listen HAL that voice call is inactive
    if (mListenHw) {
        mListenHw->notifyEvent(AUDIO_CAPTURE_INACTIVE);
    }
#endif

#ifdef QCOM_USBAUDIO_ENABLED
   if(musbPlaybackState & USBPLAYBACKBIT_VOICECALL) {
          ALOGD("Voice call ended on USB");
          musbPlaybackState &= ~USBPLAYBACKBIT_VOICECALL;
          musbRecordingState &= ~USBRECBIT_VOICECALL;
          closeUsbRecordingIfNothingActive();
          closeUsbPlaybackIfNothingActive();
   }
#endif
}

void AudioHardwareALSA::enableVoiceCall(int mode, int device, uint32_t vsid)
{
    // Start voice call
    unsigned long bufferSize = DEFAULT_VOICE_BUFFER_SIZE;
    alsa_handle_t alsa_handle;
    char *use_case;
    char *verb = getUcmVerbForVSID(vsid);
    char *modifier = getUcmModForVSID(vsid);

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    //Notify to listen HAL that voice call is active
    if (mListenHw) {
        mListenHw->notifyEvent(AUDIO_CAPTURE_ACTIVE);
    }
#endif

    if (verb == NULL || modifier == NULL) {
        ALOGE("%s(): Error, verb=%p or modifier=%p is NULL",
              __func__, verb, modifier);

            return;
    }

    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strlcpy(alsa_handle.useCase, verb, sizeof(alsa_handle.useCase));
    } else {
        strlcpy(alsa_handle.useCase, modifier, sizeof(alsa_handle.useCase));
    }
    free(use_case);

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
    bufferSize &= ~b;
    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = device;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = VOICE_CHANNEL_MODE;
    alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.ucMgr = mUcMgr;
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    setInChannels(device);

    ALOGV("%s: enable Voice call voice_vsid:%x", __func__, vsid);

    mALSADevice->route(&(*it), (uint32_t)device, mode);
    if (!strcmp(it->useCase, verb)) {
        snd_use_case_set(mUcMgr, "_verb", verb);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", modifier);
    }
    mALSADevice->startVoiceCall(&(*it), vsid);

#ifdef QCOM_USBAUDIO_ENABLED
    if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
       (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
       startUsbRecordingIfNotStarted();
       startUsbPlaybackIfNotStarted();
       musbPlaybackState |= USBPLAYBACKBIT_VOICECALL;
       musbRecordingState |= USBRECBIT_VOICECALL;
    }
#endif
}

char *AudioHardwareALSA::getUcmVerbForVSID(uint32_t vsid)
{
    char *verb = NULL;

    switch (vsid) {
       case VOLTE_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOLTE;
           break;

       case VOICE2_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOICE2;
           break;

       case VOICE_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOICECALL;
           break;

        default:
            ALOGE("%s: Invalid vsid:%x", __func__, vsid);
    }

    return verb;
}

char *AudioHardwareALSA::getUcmModForVSID(uint32_t vsid)
{
    char *mod = NULL;

    switch (vsid) {
    case VOLTE_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOLTE;
        break;

    case VOICE2_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOICE2;
        break;

    case VOICE_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOICE;
        break;

    default:
        ALOGE("%s: Invalid vsid:%x", __func__, vsid);
    }

    return mod;
}

int *AudioHardwareALSA::getCallStateForVSID(uint32_t vsid)
{
    int *callState = NULL;

    switch (vsid) {
    case VOLTE_SESSION_VSID:
        callState = &mVolteCallState;
        break;

    case VOICE2_SESSION_VSID:
        callState = &mVoice2CallState;
        break;

    case VOICE_SESSION_VSID:
        callState = &mVoiceCallState;
        break;

    default:
       ALOGE("%s: Invalid vsid:%x", __func__, vsid);
       callState = NULL;
    }

    return callState;
}

alsa_handle_t *AudioHardwareALSA::getALSADeviceHandleForVSID(uint32_t vsid)
{
    alsa_handle_t *handle = NULL;
    char *ucmVerbForCall = getUcmVerbForVSID(vsid);
    char *ucmModForCall =  getUcmModForVSID(vsid);
    ALSAHandleList::iterator vt_it;

    if (ucmVerbForCall == NULL || ucmModForCall == NULL ) {
        ALOGE("%s: Error, ucmVerbForCall=%p or ucmModForCall=%p is NULL",
              __func__, ucmVerbForCall, ucmModForCall);

        return handle;
    }

    for(vt_it = mDeviceList.begin();
         vt_it != mDeviceList.end(); ++vt_it) {
        if((!strncmp(vt_it->useCase, ucmVerbForCall,
                     MAX(strlen(ucmVerbForCall), strlen(vt_it->useCase)))) ||
           (!strncmp(vt_it->useCase, ucmModForCall,
                     MAX(strlen(ucmModForCall), strlen(vt_it->useCase))))) {
            handle = (alsa_handle_t *)(&(*vt_it));
            break;
        }
    }

    return handle;
}

bool AudioHardwareALSA::routeCall(int device, int newMode, uint32_t vsid)
{
    bool isRouted = false;
    alsa_handle_t *handle = NULL;
    int err = 0;
    int *curCallState = getCallStateForVSID(vsid);
    int newCallState = mCallState;
    enum voice_lch_mode lch_mode;

    if (curCallState == NULL) {
        ALOGE("%s(): Error, mCurCallState=%p is NULL",
              __func__, curCallState);

        return isRouted;
    }

    ALOGV("%s: CurCallState=%x newCallState=%x, vsid =%x",
          __func__, *curCallState, newCallState, vsid);


    switch (newCallState) {
    case CALL_INACTIVE:
        if (*curCallState != CALL_INACTIVE) {
            ALOGV("%s: Disabling call for vsid:%x", __func__, vsid);

            disableVoiceCall(newMode, device, vsid);
            isRouted = true;
            *curCallState = CALL_INACTIVE;
        } else {
            ALOGV("%s(): NO-OP in CALL_INACTIVE for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_ACTIVE:
        if (*curCallState == CALL_INACTIVE) {
            ALOGV("%s(): Start call for vsid:%x ",__func__, vsid);

            enableVoiceCall(newMode, device, vsid);
            isRouted = true;
            *curCallState = CALL_ACTIVE;

        } else if (*curCallState == CALL_HOLD) {
            ALOGV("%s(): Resume call from hold state for vsid:%x",
                  __func__, vsid);

            *curCallState = CALL_ACTIVE;
            isRouted = true;
        } else if(*curCallState == CALL_LOCAL_HOLD) {
            ALOGV("%s: Resume call from local call hold state \
                   for vsid:%x",__func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                lch_mode = VOICE_LCH_STOP;
                if (ioctl((int)handle->handle->fd,
                    SNDRV_VOICE_IOCTL_LCH, &lch_mode) < 0) {
                    ALOGE("Call resume failed");
                }

                *curCallState = CALL_ACTIVE;
                isRouted = true;
            } else {
                ALOGE("%s: AlsaHandle for vsid=%d is NULL", __func__, vsid);
            }
        } else {
            ALOGV("%s():NO-OP in CALL_ACTIVE for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_HOLD:
        if (*curCallState == CALL_ACTIVE ||
            *curCallState == CALL_LOCAL_HOLD) {
            ALOGV("%s(): Call going to Hold for vsid:%x",__func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                if (*curCallState == CALL_LOCAL_HOLD) {
                    lch_mode = VOICE_LCH_STOP;
                    if (ioctl((int)handle->handle->fd,
                        SNDRV_VOICE_IOCTL_LCH, &lch_mode) < 0)
                    {
                        ALOGE("%s(): Voice LCH disable failed for vsid=%x",
                              __func__, vsid);

                        break;
                    }
                }

                *curCallState = CALL_HOLD;
                isRouted = true;
            } else {
                ALOGE("%s(): AlsaHandle for vsid=%d is NULL", __func__, vsid);
            }
        } else {
            ALOGV("%s(): NO-OP in CALL_HOLD for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_LOCAL_HOLD:
        if (*curCallState == CALL_ACTIVE || *curCallState == CALL_HOLD) {
            ALOGV("%s(): Call going to local Hold for vsid:%x", __func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                lch_mode = VOICE_LCH_START;
                if (ioctl((int)handle->handle->fd,
                    SNDRV_VOICE_IOCTL_LCH, &lch_mode) < 0) {
                    ALOGE("%s(): LCH failed for vsid=%x",__func__, vsid);
                }

                *curCallState = CALL_LOCAL_HOLD;
                isRouted = true;
            } else {
                ALOGE("%s(): AlsaHandle for vsid=%x is NULL",__func__, vsid);
            }
        } else {
            ALOGV("%s(): NO-OP in CALL_LOCAL_HOLD for vsid:%x",__func__, vsid);
        }
        break;

       default:
       case CALL_INVALID:
           ALOGE("%s(): Invalid Call state=%d vsid=%x",
                 __func__, newCallState, vsid );
           break;
    }

    return isRouted;
}

#ifdef QCOM_TUNNEL_LPA_ENABLED
void AudioHardwareALSA::pauseIfUseCaseTunnelOrLPA() {
    for (ALSAHandleList::iterator it = mDeviceList.begin();
           it != mDeviceList.end(); it++) {
        if ((isTunnelUseCase(it->useCase)) ||
            (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
                it->session->pause_l();
        }
    }
}

void AudioHardwareALSA::resumeIfUseCaseTunnelOrLPA() {
    for (ALSAHandleList::iterator it = mDeviceList.begin();
           it != mDeviceList.end(); it++) {
        if ((isTunnelUseCase(it->useCase)) ||
            (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
                it->session->resume_l();
        }
    }
}
#endif

status_t AudioHardwareALSA::startPlaybackOnExtOut(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    status_t err = startPlaybackOnExtOut_l(activeUsecase);
    if(err) {
        ALOGE("startPlaybackOnExtOut_l  = %d", err);
    }
    return err;
}
status_t AudioHardwareALSA::startPlaybackOnExtOut_l(uint32_t activeUsecase) {

    ALOGV("startPlaybackOnExtOut_l::usecase = %d ", activeUsecase);
    status_t err = NO_ERROR;
    if (!mExtOutStream) {
        ALOGE("Unable to open ExtOut stream");
        return err;
    }
    if (activeUsecase != USECASE_NONE && !mIsExtOutEnabled) {
        Mutex::Autolock autolock1(mExtOutMutex);
         err = setProxyProperty(0);
         if(err) {
            ALOGE("Proxy Property Set Failedd");
        }
        int ProxyOpenRetryCount=PROXY_OPEN_RETRY_COUNT;
        while(ProxyOpenRetryCount){
            err = mALSADevice->openProxyDevice();
            if(err) {
                 ProxyOpenRetryCount --;
                 usleep(PROXY_OPEN_WAIT_TIME * 1000);
                 ALOGV("openProxyDevice failed retrying = %d", ProxyOpenRetryCount);
            }
            else
               break;
        }
        if(err) {
            ALOGE("openProxyDevice failed = %d", err);
        }

        mKillExtOutThread = false;
        err = pthread_create(&mExtOutThread, (const pthread_attr_t *) NULL,
                extOutThreadWrapper,
                this);
        if(err) {
            ALOGE("thread create failed = %d", err);
            return err;
        }
        mExtOutThreadAlive = true;
        mIsExtOutEnabled = true;

#ifdef OUTPUT_BUFFER_LOG
    sprintf(outputfilename, "%s%d%s", outputfilename, number,".pcm");
    outputBufferFile1 = fopen (outputfilename, "ab");
    number++;
#endif
    }

    setExtOutActiveUseCases_l(activeUsecase);
    mALSADevice->resumeProxy();

    Mutex::Autolock autolock1(mExtOutMutex);
    ALOGV("ExtOut signal");
    mExtOutCv.signal();
    return err;
}

status_t AudioHardwareALSA::setProxyProperty(uint32_t value) {
     status_t err=NO_ERROR;
     if(value){
        //property_set("proxy.can.open", "true");
        mCanOpenProxy = 1;
     }
     else{
        //property_set("proxy.can.open", "false");
        mCanOpenProxy = 0;
     }
     return err;
}


status_t AudioHardwareALSA::stopPlaybackOnExtOut(uint32_t activeUsecase) {
     Mutex::Autolock autoLock(mLock);
     status_t err = stopPlaybackOnExtOut_l(activeUsecase);
     if(err) {
         ALOGE("stopPlaybackOnExtOut = %d", err);
     }
     return err;
}

status_t AudioHardwareALSA::stopPlaybackOnExtOut_l(uint32_t activeUsecase) {

     ALOGV("stopPlaybackOnExtOut  = %d", activeUsecase);
     status_t err = NO_ERROR;
     suspendPlaybackOnExtOut_l(activeUsecase);
     {
         Mutex::Autolock autolock1(mExtOutMutex);
         ALOGV("stopPlaybackOnExtOut  getExtOutActiveUseCases_l = %d",
                getExtOutActiveUseCases_l());

         if(!getExtOutActiveUseCases_l()) {
             mIsExtOutEnabled = false;

             mExtOutMutex.unlock();
             err = stopExtOutThread();
             if(err) {
                 ALOGE("stopExtOutThread Failed :: err = %d" ,err);
             }
             mExtOutMutex.lock();

             if (mExtOutStream != NULL) {
                 ALOGV(" External Output Stream Standby called");
                 mExtOutStream->common.standby(&mExtOutStream->common);
             }

             err = mALSADevice->closeProxyDevice();
             if(err) {
                 ALOGE("closeProxyDevice failed = %d", err);
             }
             err = setProxyProperty(1);
             mExtOutActiveUseCases = 0x0;
             mRouteAudioToExtOut = false;

#ifdef OUTPUT_BUFFER_LOG
    ALOGV("close file output");
    if(outputBufferFile1)
        fclose (outputBufferFile1);
#endif
         }
     }
     return err;
}

status_t AudioHardwareALSA::openExtOutput(int device) {

    ALOGV("openExtOutput");
    status_t err = NO_ERROR;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        err= openA2dpOutput();
        if(err) {
            ALOGE("openA2DPOutput failed = %d",err);
            return err;
        }
        if(!mExtOutStream) {
            mExtOutStream = mA2dpStream;
        }
#ifdef QCOM_USBAUDIO_ENABLED
    } else if (device & AudioSystem::DEVICE_OUT_ALL_USB) {
        err= openUsbOutput();
        if(err) {
            ALOGE("openUsbPOutput failed = %d",err);
            return err;
        }
        if(!mExtOutStream) {
            mExtOutStream = mUsbStream;
        }
#endif
    }
    return err;
}

status_t AudioHardwareALSA::closeExtOutput(int device) {

    ALOGD("closeExtOutput");
    status_t err = NO_ERROR;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        if(mExtOutStream == mA2dpStream)
            mExtOutStream = NULL;
        err= closeA2dpOutput();
        if(err) {
            ALOGE("closeA2DPOutput failed = %d",err);
            return err;
        }
    } else if (device & AUDIO_DEVICE_OUT_ALL_USB) {
        if(mExtOutStream == mUsbStream)
            mExtOutStream = NULL;
        err= closeUsbOutput();
        if(err) {
            ALOGE("closeUsbPOutput failed = %d",err);
            return err;
        }
    }
    return err;
}

status_t AudioHardwareALSA::openA2dpOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = AFE_PROXY_SAMPLE_RATE;
    status_t status;
    ALOGD("openA2dpOutput");
    struct audio_config config;
    config.sample_rate = AFE_PROXY_SAMPLE_RATE;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_PCM_16_BIT;

    //TODO : Confirm AUDIO_HARDWARE_MODULE_ID_A2DP ???
    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID/*_A2DP*/, (const char*)"a2dp",
                                    (const hw_module_t**)&mod);
    if (rc) {
        ALOGE("Could not get a2dp hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mA2dpDevice);
    if(rc) {
        ALOGE("couldn't open a2dp audio hw device");
        return NO_INIT;
    }
    //TODO: unique id 0?
    status = mA2dpDevice->open_output_stream(mA2dpDevice, 0,((audio_devices_t)(AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP)),
                                    (audio_output_flags_t)AUDIO_OUTPUT_FLAG_NONE, &config, &mA2dpStream);
    if(status != NO_ERROR) {
        ALOGE("Failed to open output stream for a2dp: status %d", status);
    }
    return status;
}

status_t AudioHardwareALSA::closeA2dpOutput()
{
    ALOGD("closeA2dpOutput");
    if(!mA2dpDevice){
        ALOGE("No Aactive A2dp output found");
        return NO_ERROR;
    }

    mA2dpDevice->close_output_stream(mA2dpDevice, mA2dpStream);
    mA2dpStream = NULL;

    audio_hw_device_close(mA2dpDevice);
    mA2dpDevice = NULL;
    return NO_ERROR;
}

status_t AudioHardwareALSA::openUsbOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = AFE_PROXY_SAMPLE_RATE;
    status_t status;
    ALOGD("openUsbOutput");
    struct audio_config config;
    config.sample_rate = AFE_PROXY_SAMPLE_RATE;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_PCM_16_BIT;

    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID/*_USB*/, (const char*)"usb",
                                    (const hw_module_t**)&mod);
    if (rc) {
        ALOGE("Could not get usb hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mUsbDevice);
    if(rc) {
        ALOGE("couldn't open Usb audio hw device");
        return NO_INIT;
    }

    status = mUsbDevice->open_output_stream(mUsbDevice, 0,((audio_devices_t)(AUDIO_DEVICE_OUT_USB_ACCESSORY)),
                                    (audio_output_flags_t)AUDIO_OUTPUT_FLAG_NONE, &config, &mUsbStream);
    if(status != NO_ERROR) {
        ALOGE("Failed to open output stream for USB: status %d", status);
    }

    return status;
}

status_t AudioHardwareALSA::closeUsbOutput()
{
    ALOGD("closeUsbOutput");
    if(!mUsbDevice){
        ALOGE("No Aactive Usb output found");
        return NO_ERROR;
    }

    mUsbDevice->close_output_stream(mUsbDevice, mUsbStream);
    mUsbStream = NULL;

    audio_hw_device_close(mUsbDevice);
    mUsbDevice = NULL;
    return NO_ERROR;
}

status_t AudioHardwareALSA::stopExtOutThread()
{
    ALOGD("stopExtOutThread");
    status_t err = NO_ERROR;
    if (!mExtOutThreadAlive) {
        ALOGD("Return - thread not live");
        return NO_ERROR;
    }
    mExtOutMutex.lock();
    mKillExtOutThread = true;
    err = mALSADevice->exitReadFromProxy();
    if(err) {
        ALOGE("exitReadFromProxy failed = %d", err);
    }
    mExtOutCv.signal();
    mExtOutMutex.unlock();
    int ret = pthread_join(mExtOutThread,NULL);
    ALOGD("ExtOut thread killed = %d", ret);
    return err;
}

void AudioHardwareALSA::switchExtOut(int device) {

    ALOGD("switchExtOut");
    uint32_t sampleRate;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        mExtOutStream = mA2dpStream;
    } else if (device & AUDIO_DEVICE_OUT_ALL_USB) {
        mExtOutStream = mUsbStream;
    } else {
        mExtOutStream = NULL;
    }
    if ((mExtOutStream == mUsbStream) && mExtOutStream != NULL) {
        sampleRate = mExtOutStream->common.get_sample_rate(&mExtOutStream->common);
        if (sampleRate > AFE_PROXY_SAMPLE_RATE) {
            ALOGW(" Requested samplerate %d is greater than supported so fall back to %d ",
                  sampleRate,AFE_PROXY_SAMPLE_RATE);
            sampleRate = AFE_PROXY_SAMPLE_RATE;
        }
        if (mResampler != NULL) {
            release_resampler(mResampler);
            mResampler = NULL;
        }
        if (sampleRate != AFE_PROXY_SAMPLE_RATE) {
            mResampler = (struct resampler_itfe *)calloc(1, sizeof(struct resampler_itfe));
            if (mResampler != NULL ) {
                status_t err = create_resampler(AFE_PROXY_SAMPLE_RATE,
                                                sampleRate,
                                                2, //channel count
                                                RESAMPLER_QUALITY_DEFAULT,
                                                NULL,
                                                &mResampler);
                ALOGD(" sampleRate %d mResampler %p",sampleRate,mResampler);
                if (err) {
                    ALOGE(" Failed to create resampler");
                    free(mResampler);
                    mResampler = NULL;
                }
            } else{
                ALOGE(" Failed to allocate memory for mResampler = %p",mResampler);
            }
        }
    }
}

status_t AudioHardwareALSA::isExtOutDevice(int device) {
    return ((device & AudioSystem::DEVICE_OUT_ALL_A2DP) ||
            (device & AUDIO_DEVICE_OUT_ALL_USB)) ;
}

void *AudioHardwareALSA::extOutThreadWrapper(void *me) {
    static_cast<AudioHardwareALSA *>(me)->extOutThreadFunc();
    return NULL;
}

void AudioHardwareALSA::extOutThreadFunc() {
    if(!mExtOutStream) {
        ALOGE("No valid External output stream found");
        return;
    }
    if(!mALSADevice->isProxyDeviceOpened()) {
        ALOGE("No valid mProxyPcmHandle found");
        return;
    }

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"ExtOutThread", 0, 0, 0);

    int ionBufCount = 0;
    int32_t bytesWritten = 0;
    uint32_t numBytesRemaining = 0;
    uint32_t bytesAvailInBuffer = 0;
    uint32_t proxyBufferTime = 0;
    void  *data;
    int err = NO_ERROR;
    ssize_t size = 0;
    void * outbuffer= malloc(AFE_PROXY_PERIOD_SIZE);

    mALSADevice->resetProxyVariables();

    ALOGD("mKillExtOutThread = %d", mKillExtOutThread);
    while(!mKillExtOutThread) {
        {
            Mutex::Autolock autolock1(mExtOutMutex);
            if (mKillExtOutThread) {
                break;
            }
            if (!mExtOutStream || !mIsExtOutEnabled ||
                !mALSADevice->isProxyDeviceOpened() ||
                (mALSADevice->isProxyDeviceSuspended()) ||
                (err != NO_ERROR)) {
                ALOGD("ExtOutThreadEntry:: proxy opened = %d,\
                        proxy suspended = %d,err =%d,\
                        mExtOutStream = %p mIsExtOutEnabled = %d",\
                        mALSADevice->isProxyDeviceOpened(),\
                        mALSADevice->isProxyDeviceSuspended(),err,mExtOutStream, mIsExtOutEnabled);
                ALOGD("ExtOutThreadEntry:: Waiting on mExtOutCv");
                mExtOutCv.wait(mExtOutMutex);
                ALOGD("ExtOutThreadEntry:: received signal to wake up");
                mExtOutMutex.unlock();
                continue;
            }
        }
        err = mALSADevice->readFromProxy(&data, &size);
        if(err < 0) {
           ALOGE("ALSADevice readFromProxy returned err = %d,data = %p,\
                    size = %ld", err, data, size);
           continue;
        }

#ifdef OUTPUT_BUFFER_LOG
    if (outputBufferFile1)
    {
        fwrite (data,1,size,outputBufferFile1);
    }
#endif
        void *copyBuffer = data;
        numBytesRemaining = size;
        proxyBufferTime = mALSADevice->mProxyParams.mBufferTime;
        {
            Mutex::Autolock autolock1(mExtOutMutex);
            if (mResampler != NULL && (mUsbStream == mExtOutStream)) {
                uint32_t inFrames = size/(AFE_PROXY_CHANNEL_COUNT*2);
                uint32_t outFrames = inFrames;
                mResampler->resample_from_input(mResampler,
                                               (int16_t *)data,
                                               &inFrames,
                                               (int16_t *)outbuffer,
                                               &outFrames);
                copyBuffer = outbuffer;
                numBytesRemaining = outFrames*(AFE_PROXY_CHANNEL_COUNT*2);
                ALOGV("inFrames %d outFrames %d",inFrames,outFrames);
            }
        }
        while (err == OK && (numBytesRemaining  > 0) && !mKillExtOutThread
                && mIsExtOutEnabled ) {
            {
                mExtOutMutex.lock();
                if(mExtOutStream != NULL ) {
                    bytesAvailInBuffer = mExtOutStream->common.get_buffer_size(&mExtOutStream->common);
                    uint32_t writeLen = bytesAvailInBuffer > numBytesRemaining ?
                                    numBytesRemaining : bytesAvailInBuffer;
                    ALOGV("Writing %d bytes to External Output ", writeLen);
                    bytesWritten = mExtOutStream->write(mExtOutStream,copyBuffer, writeLen);
                } else {
                    //unlock the mutex before sleep
                    mExtOutMutex.unlock();
                    ALOGV(" No External output to write  ");
                    usleep(proxyBufferTime*1000);
                    bytesWritten = numBytesRemaining;
                }
                mExtOutMutex.unlock();
            }
            //If the write fails make this thread sleep and let other
            //thread (eg: stopA2DP) to acquire lock to prevent a deadlock.
            if(bytesWritten == -1 || bytesWritten == 0) {
                ALOGV("bytesWritten = %d",bytesWritten);
                usleep(10000);
                break;
            }
            //Need to check warning here - void used in arithmetic
            copyBuffer = (char *)copyBuffer + bytesWritten;
            numBytesRemaining -= bytesWritten;
            ALOGV("@_@bytes To write2:%d",numBytesRemaining);
        }
    }

    mALSADevice->resetProxyVariables();
    mExtOutThreadAlive = false;
    ALOGD("ExtOut Thread is dying");
}

void AudioHardwareALSA::setExtOutActiveUseCases_l(uint32_t activeUsecase)
{
   mExtOutActiveUseCases |= activeUsecase;
   ALOGD("mExtOutActiveUseCases = %u, activeUsecase = %u", mExtOutActiveUseCases, activeUsecase);
}

uint32_t AudioHardwareALSA::getExtOutActiveUseCases_l()
{
   ALOGD("getExtOutActiveUseCases_l: mExtOutActiveUseCases = %u", mExtOutActiveUseCases);
   return mExtOutActiveUseCases;
}

void AudioHardwareALSA::clearExtOutActiveUseCases_l(uint32_t activeUsecase) {

   mExtOutActiveUseCases &= ~activeUsecase;
   ALOGD("clear - mExtOutActiveUseCases = %u, activeUsecase = %u", mExtOutActiveUseCases, activeUsecase);

}

uint32_t AudioHardwareALSA::useCaseStringToEnum(const char *usecase)
{
   ALOGV("useCaseStringToEnum usecase:%s",usecase);
   uint32_t activeUsecase = USECASE_NONE;

   if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                    strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
       (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_LPA,
                    strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
       activeUsecase = USECASE_HIFI_LOW_POWER;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
              (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL)))) {
       activeUsecase = USECASE_HIFI_TUNNEL;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
              (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL2)))) {
       activeUsecase = USECASE_HIFI_TUNNEL2;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL3,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL3))) ||
              (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL3,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL3)))) {
       activeUsecase = USECASE_HIFI_TUNNEL3;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL4,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL4))) ||
              (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL4,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL4)))) {
       activeUsecase = USECASE_HIFI_TUNNEL4;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC))) ||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC)))) {
       activeUsecase = USECASE_HIFI_LOWLATENCY;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_DIGITAL_RADIO,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_DIGITAL_RADIO))) ||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_FM,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_FM))) ||
               (!strncmp(usecase, SND_USE_CASE_VERB_FM_REC,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_FM_REC))) ||
               (!strncmp(usecase, SND_USE_CASE_MOD_CAPTURE_FM,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_CAPTURE_FM)))){
       activeUsecase = USECASE_FM;
    } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI,
                           MAX_LEN(usecase, SND_USE_CASE_VERB_HIFI)))||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_MUSIC,
                           MAX_LEN(usecase, SND_USE_CASE_MOD_PLAY_MUSIC)))) {
       activeUsecase = USECASE_HIFI;
    }
    return activeUsecase;
}

bool  AudioHardwareALSA::suspendPlaybackOnExtOut(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    suspendPlaybackOnExtOut_l(activeUsecase);
    return NO_ERROR;
}

bool  AudioHardwareALSA::suspendPlaybackOnExtOut_l(uint32_t activeUsecase) {

    Mutex::Autolock autolock1(mExtOutMutex);
    ALOGD("suspendPlaybackOnExtOut_l activeUsecase = %d, mRouteAudioToExtOut = %d",\
            activeUsecase, mRouteAudioToExtOut);
    clearExtOutActiveUseCases_l(activeUsecase);
    if((!getExtOutActiveUseCases_l()) && mIsExtOutEnabled )
        return mALSADevice->suspendProxy();
    return NO_ERROR;
}

#ifdef QCOM_DS1_DOLBY_DDP
status_t AudioHardwareALSA::setDDPEndpParams(int device)
{
    ALOGV("%s E", __func__);
    int dev_ch_cap = 2, idx;
    EDID_AUDIO_INFO info = { 0 };

    for(ALSAHandleList::iterator it = mDeviceList.begin(); it != mDeviceList.end(); it++) {
        if (isTunnelUseCase(it->useCase)) {
            if ((it->format == AUDIO_FORMAT_EAC3 ||
                 it->format == AUDIO_FORMAT_AC3) &&
                (it->handle)) {
                if(device & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
                    char hdmiEDIDData[MAX_SHORT_AUDIO_DESC_CNT+1];
                    if(mALSADevice->getEDIDData(hdmiEDIDData) == NO_ERROR) {
                        if (AudioUtil::getHDMIAudioSinkCaps(&info, hdmiEDIDData)) {
                            for (int i = 0; i < info.nAudioBlocks && i < MAX_EDID_BLOCKS; i++) {
                                if (info.AudioBlocksArray[i].nChannels > dev_ch_cap &&
                                    info.AudioBlocksArray[i].nChannels <= 8) {
                                    dev_ch_cap = info.AudioBlocksArray[i].nChannels;
                                }
                            }
                        }
                    }
                 }
                 char *ddpEndpParams;
                 int length;
                 ddpEndpParams = (char *) malloc (2*DDP_ENDP_NUM_PARAMS*sizeof(int));
                 if(ddpEndpParams == NULL)
                     continue;
                 mALSADevice->setDDPEndpParams(&(*it), device, dev_ch_cap,
                                                ddpEndpParams, &length, true);
                 free(ddpEndpParams);
            }
        }
    }
    return NO_ERROR;
}
#endif

}       // namespace android_audio_legacy
