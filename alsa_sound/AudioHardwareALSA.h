/* AudioHardwareALSA.h
 **
 ** Copyright 2008-2010, Wind River Systems
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

#ifndef ANDROID_AUDIO_HARDWARE_ALSA_H
#define ANDROID_AUDIO_HARDWARE_ALSA_H

#include <utils/List.h>
#include <hardware_legacy/AudioHardwareBase.h>

#include <hardware_legacy/AudioHardwareInterface.h>
#include <hardware_legacy/AudioSystemLegacy.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include <utils/threads.h>
#include <dlfcn.h>
#ifdef QCOM_USBAUDIO_ENABLED
#include <AudioUsbALSA.h>
#endif
#include <sys/poll.h>
#include <sys/eventfd.h>
#ifdef QCOM_LISTEN_FEATURE_ENABLE
#include "ListenHardware.h"
#endif

#ifdef RESOURCE_MANAGER
#include "AudioResourceManager.h"
#endif
extern "C" {
    #include <sound/asound.h>
    #include <sound/compress_params.h>
    #include <sound/compress_offload.h>
    #include <sound/voice_params.h>
    #include "alsa_audio.h"
    #include "msm8960_use_cases.h"
}

#include <hardware/hardware.h>

namespace android_audio_legacy
{
using android::List;
using android::Mutex;
using android::Condition;

class AudioHardwareALSA;
#ifdef RESOURCE_MANAGER
class AudioResourceManager;
#endif
/**
 * The id of ALSA module
 */
#define ALSA_HARDWARE_MODULE_ID "alsa"
#define ALSA_HARDWARE_NAME      "alsa"

#define MAX(a,b) (((a)>(b))?(a):(b))

#define MAX_SOUND_CARDS 4

#define DEFAULT_SAMPLING_RATE 48000
#define DEFAULT_CHANNEL_MODE  2
#define VOICE_SAMPLING_RATE   8000
#define VOICE_CHANNEL_MODE    1
#define PLAYBACK_LATENCY      96000
#define RECORD_LATENCY        96000
#define VOICE_LATENCY         85333
#define DEFAULT_BUFFER_SIZE   2048
#ifdef TARGET_B_FAMILY
#define DEFAULT_MULTI_CHANNEL_BUF_SIZE    6144
#else
//4032 = 336(kernel buffer size) * 2(bytes pcm_16) * 6(number of channels)
#define DEFAULT_MULTI_CHANNEL_BUF_SIZE    4032
#endif

#define DEFAULT_VOICE_BUFFER_SIZE   2048
#define PLAYBACK_LOW_LATENCY_BUFFER_SIZE   960
#define PLAYBACK_LOW_LATENCY  22000
#define PLAYBACK_LOW_LATENCY_MEASURED  42000
#ifdef TARGET_B_FAMILY
#define DEFAULT_IN_BUFFER_SIZE 512
#define MIN_CAPTURE_BUFFER_SIZE_PER_CH   512
#else
#define DEFAULT_IN_BUFFER_SIZE 320
#define MIN_CAPTURE_BUFFER_SIZE_PER_CH   320
#endif
#define VOIP_BUFFER_SIZE_8K    320
#define VOIP_BUFFER_SIZE_16K   640
#define MAX_CAPTURE_BUFFER_SIZE_PER_CH   2048
#define FM_BUFFER_SIZE        1024
#define AMR_WB_FRAMESIZE      61

#define VOIP_SAMPLING_RATE_8K 8000
#define VOIP_SAMPLING_RATE_16K 16000
#define VOIP_DEFAULT_CHANNEL_MODE  1
#define VOIP_BUFFER_MAX_SIZE   VOIP_BUFFER_SIZE_16K
#define VOIP_PLAYBACK_LATENCY      6400
#define VOIP_RECORD_LATENCY        6400

#define MODE_IS127              0x2
#define MODE_4GV_NB             0x3
#define MODE_4GV_WB             0x4
#define MODE_AMR                0x5
#define MODE_AMR_WB             0xD
#define MODE_PCM                0xC
#define MODE_4GV_NW             0xE

#define DUALMIC_KEY         "dualmic_enabled"
#define FLUENCE_KEY         "fluence"
#define VOIPCHECK_KEY         "voip_flag"
#define ANC_KEY             "anc_enabled"
#define TTY_MODE_KEY        "tty_mode"
#define BT_SAMPLERATE_KEY   "bt_samplerate"
#define BTHEADSET_VGS       "bt_headset_vgs"
#define WIDEVOICE_KEY       "wide_voice_enable"
#define VOIPRATE_KEY        "voip_rate"
#define FENS_KEY            "fens_enable"
#define ST_KEY              "st_enable"
#define INCALLMUSIC_KEY     "incall_music_enabled"
#define VSID_KEY            "vsid"
#define CALL_STATE_KEY      "call_state"
#define VOLUME_BOOST_KEY    "volume_boost"
#define AUDIO_PARAMETER_KEY_FM_VOLUME "fm_volume"
#define ECHO_SUPRESSION     "ec_supported"
#define ALL_CALL_STATES_KEY "all_call_states"
#define CUSTOM_STEREO_KEY   "stereo_as_dual_mono"
#define VOIP_DTX_MODE_KEY   "dtx_on"
#define EVRC_RATE_MIN_KEY   "evrc_rate_min"
#define EVRC_RATE_MAX_KEY   "evrc_rate_max"

#define ANC_FLAG        0x00000001
#define DMIC_FLAG       0x00000002
#define QMIC_FLAG       0x00000004
#ifdef QCOM_SSR_ENABLED
#define SSRQMIC_FLAG    0x00000008
#endif

#define TTY_OFF         0x00000010
#define TTY_FULL        0x00000020
#define TTY_VCO         0x00000040
#define TTY_HCO         0x00000080
#define TTY_CLEAR       0xFFFFFF0F

#define LPA_SESSION_ID 1
#define TUNNEL_SESSION_ID 2
#ifdef QCOM_USBAUDIO_ENABLED
#define PROXY_OPEN_WAIT_TIME  20
#define PROXY_OPEN_RETRY_COUNT 100

static int USBPLAYBACKBIT_MUSIC = (1 << 0);
static int USBPLAYBACKBIT_VOICECALL = (1 << 1);
static int USBPLAYBACKBIT_VOIPCALL = (1 << 2);
static int USBPLAYBACKBIT_FM = (1 << 3);
static int USBPLAYBACKBIT_LPA = (1 << 4);
static int USBPLAYBACKBIT_TUNNEL = (1 << 5);
static int USBPLAYBACKBIT_TUNNEL2 = (1 << 6);
static int USBPLAYBACKBIT_TUNNEL3 = (1 << 7);
static int USBPLAYBACKBIT_TUNNEL4 = (1 << 8);
static int USBPLAYBACKBIT_MULTICHANNEL = (1 << 9);
static int USBPLAYBACKBIT_LOWLATENCY = (1 << 10);

static int USBRECBIT_REC = (1 << 0);
static int USBRECBIT_VOICECALL = (1 << 1);
static int USBRECBIT_VOIPCALL = (1 << 2);
static int USBRECBIT_FM = (1 << 3);
#endif

#define DEVICE_SPEAKER_HEADSET "Speaker Headset"
#define DEVICE_HEADSET "Headset"
#define DEVICE_HEADPHONES "Headphones"

#ifdef QCOM_SSR_ENABLED
#define COEFF_ARRAY_SIZE          4
#define FILT_SIZE                 ((512+1)* 6)    /* # ((FFT bins)/2+1)*numOutputs */
#define SSR_FRAME_SIZE            512
#define SSR_INPUT_FRAME_SIZE      (SSR_FRAME_SIZE * 4)
#define SSR_OUTPUT_FRAME_SIZE     (SSR_FRAME_SIZE * 6)
#endif

#define MODE_CALL_KEY  "CALL_KEY"
#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define NUM_FDS 2
#define AFE_PROXY_SAMPLE_RATE 48000
#define AFE_PROXY_CHANNEL_COUNT 2
#define AFE_PROXY_PERIOD_SIZE 3072

#define MAX_SLEEP_RETRY 100  /*  Will check 100 times before continuing */
#define AUDIO_INIT_SLEEP_WAIT 50 /* 50 ms */

/* Front left channel. */
#define PCM_CHANNEL_FL    1
/* Front right channel. */
#define PCM_CHANNEL_FR    2
/* Front center channel. */
#define PCM_CHANNEL_FC    3
/* Left surround channel.*/
#define PCM_CHANNEL_LS   4
/* Right surround channel.*/
#define PCM_CHANNEL_RS   5
/* Low frequency effect channel. */
#define PCM_CHANNEL_LFE  6
/* Center surround channel; Rear center channel. */
#define PCM_CHANNEL_CS   7
/* Left back channel; Rear left channel. */
#define PCM_CHANNEL_LB   8
/* Right back channel; Rear right channel. */
#define PCM_CHANNEL_RB   9
/* Top surround channel. */
#define PCM_CHANNEL_TS   10
/* Center vertical height channel.*/
#define PCM_CHANNEL_CVH  11
/* Mono surround channel.*/
#define PCM_CHANNEL_MS   12
/* Front left of center. */
#define PCM_CHANNEL_FLC  13
/* Front right of center. */
#define PCM_CHANNEL_FRC  14
/* Rear left of center. */
#define PCM_CHANNEL_RLC  15
/* Rear right of center. */
#define PCM_CHANNEL_RRC  16

#define SOUND_CARD_SLEEP_RETRY 15  /*  Will check 5 times before continuing */
#define SOUND_CARD_SLEEP_WAIT 1000 /* 100 ms */

#ifdef QCOM_DS1_DOLBY_DDP
#define PARAM_ID_MAX_OUTPUT_CHANNELS    0x00010DE2
#define PARAM_ID_CTL_RUNNING_MODE       0x0
#define PARAM_ID_CTL_ERROR_CONCEAL      0x00010DE3
#define PARAM_ID_CTL_ERROR_MAX_RPTS     0x00010DE4
#define PARAM_ID_CNV_ERROR_CONCEAL      0x00010DE5
#define PARAM_ID_CTL_SUBSTREAM_SELECT   0x00010DE6
#define PARAM_ID_CTL_INPUT_MODE         0x0
#define PARAM_ID_OUT_CTL_OUTMODE        0x00010DE0
#define PARAM_ID_OUT_CTL_OUTLFE_ON      0x00010DE1
#define PARAM_ID_OUT_CTL_COMPMODE       0x00010D74
#define PARAM_ID_OUT_CTL_STEREO_MODE    0x00010D76
#define PARAM_ID_OUT_CTL_DUAL_MODE      0x00010D75
#define PARAM_ID_OUT_CTL_DRCSCALE_HIGH  0x00010D7A
#define PARAM_ID_OUT_CTL_DRCSCALE_LOW   0x00010D79
#define PARAM_ID_OUT_CTL_OUT_PCMSCALE   0x00010D78
#define PARAM_ID_OUT_CTL_MDCT_BANDLIMIT 0x00010DE7
#define PARAM_ID_OUT_CTL_DRC_SUPPRESS   0x00010DE8

/* DS1-DDP Endp Params */
#define DDP_ENDP_NUM_PARAMS 17
#define DDP_ENDP_NUM_DEVICES 22
static int mDDPEndpParamsId[DDP_ENDP_NUM_PARAMS] = {
    PARAM_ID_MAX_OUTPUT_CHANNELS, PARAM_ID_CTL_RUNNING_MODE,
    PARAM_ID_CTL_ERROR_CONCEAL, PARAM_ID_CTL_ERROR_MAX_RPTS,
    PARAM_ID_CNV_ERROR_CONCEAL, PARAM_ID_CTL_SUBSTREAM_SELECT,
    PARAM_ID_CTL_INPUT_MODE, PARAM_ID_OUT_CTL_OUTMODE,
    PARAM_ID_OUT_CTL_OUTLFE_ON, PARAM_ID_OUT_CTL_COMPMODE,
    PARAM_ID_OUT_CTL_STEREO_MODE, PARAM_ID_OUT_CTL_DUAL_MODE,
    PARAM_ID_OUT_CTL_DRCSCALE_HIGH, PARAM_ID_OUT_CTL_DRCSCALE_LOW,
    PARAM_ID_OUT_CTL_OUT_PCMSCALE, PARAM_ID_OUT_CTL_MDCT_BANDLIMIT,
    PARAM_ID_OUT_CTL_DRC_SUPPRESS
};
/*the table should be accessed in ALSADevice.cpp only*/
static struct mDDPEndpParams {
    int  device;
    int  dev_ch_cap;
    int  param_val[DDP_ENDP_NUM_PARAMS];
    bool is_param_valid[DDP_ENDP_NUM_PARAMS];
} mDDPEndpParams[DDP_ENDP_NUM_DEVICES] = {
          {AudioSystem::DEVICE_OUT_EARPIECE, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0 } },
          {AudioSystem::DEVICE_OUT_SPEAKER, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_WIRED_HEADSET, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_WIRED_HEADPHONE, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_SCO, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_AUX_DIGITAL, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 2, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_AUX_DIGITAL, 6,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 2, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_AUX_DIGITAL, 8,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 2, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
#ifdef QCOM_USBAUDIO_ENABLED
          {AudioSystem::DEVICE_OUT_USB_ACCESSORY, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_USB_DEVICE, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
#endif
#ifdef QCOM_FM_ENABLED
          {AudioSystem::DEVICE_OUT_FM, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_FM_TX, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
#endif
#ifdef QCOM_ANC_HEADSET_ENABLED
          {AudioSystem::DEVICE_OUT_ANC_HEADSET, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
          {AudioSystem::DEVICE_OUT_ANC_HEADPHONE, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
#endif
#ifdef QCOM_PROXY_DEVICE_ENABLED
          {AudioSystem::DEVICE_OUT_PROXY, 2,
              {8, 0, 0, 0, 0, 0, 0, 21, 1, 6, 0, 0, 0, 0, 0, 0, 0},
              {1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0} },
#endif
                       };
#endif

#define VOICE_SESSION_VSID  0x10C01000
#define VOICE2_SESSION_VSID 0x10DC1000
#define VOLTE_SESSION_VSID  0x10C02000
#define QCHAT_SESSION_VSID  0x10803000
#define ALL_SESSION_VSID    0xFFFFFFFF
#define DEFAULT_MUTE_RAMP_DURATION      500
#define DEFAULT_VOLUME_RAMP_DURATION_MS 20

static uint32_t FLUENCE_MODE_ENDFIRE   = 0;
static uint32_t FLUENCE_MODE_BROADSIDE = 1;
class ALSADevice;

enum {
    INCALL_REC_MONO,
    INCALL_REC_STEREO,
};

/* ADSP States */
enum {
    ADSP_UP = 0x0,
    ADSP_DOWN = 0x1,
    ADSP_UP_AFTER_SSR = 0x2,
};

/* Call States */
enum call_state {
    CALL_INVALID,
    CALL_INACTIVE,
    CALL_ACTIVE,
    CALL_HOLD,
    CALL_LOCAL_HOLD
};

//Audio parameter definitions

/* Query handle fm parameter*/
#define AUDIO_PARAMETER_KEY_HANDLE_FM "handle_fm"

/* Query voip flag */
#define AUDIO_PARAMETER_KEY_VOIP_CHECK "voip_flag"

/* Query Fluence type */
#define AUDIO_PARAMETER_KEY_FLUENCE_TYPE "fluence"

/* Query if surround sound recording is supported */
#define AUDIO_PARAMETER_KEY_SSR "ssr"

/* Query if a2dp  is supported */
#define AUDIO_PARAMETER_KEY_HANDLE_A2DP_DEVICE "isA2dpDeviceSupported"

/* Query ADSP Status */
#define AUDIO_PARAMETER_KEY_ADSP_STATUS "ADSP_STATUS"

/* Query if Proxy can be Opend */
#define AUDIO_CAN_OPEN_PROXY "can_open_proxy"

class AudioSessionOutALSA;
struct alsa_handle_t {
    ALSADevice*         module;
    uint32_t            devices;
    char                useCase[MAX_STR_LEN];
    struct pcm *        handle;
    snd_pcm_format_t    format;
    uint32_t            channels;
    uint32_t            sampleRate;
    int                 mode;
    unsigned int        latency;         // Delay in usec
    unsigned int        bufferSize;      // Size of sample buffer
    unsigned int        periodSize;
    bool                isFastOutput;
    struct pcm *        rxHandle;
    snd_use_case_mgr_t  *ucMgr;
#ifdef QCOM_TUNNEL_LPA_ENABLED
    AudioSessionOutALSA *session;
#endif
};

struct output_metadata_handle_t {
    uint32_t            metadataLength;
    uint32_t            bufferLength;
    uint64_t            timestamp;
    uint32_t            reserved[12];
};

typedef List < alsa_handle_t > ALSAHandleList;

struct use_case_t {
    char                useCase[MAX_STR_LEN];
};

typedef List < use_case_t > ALSAUseCaseList;
class AudioSpeakerProtection;
class ALSADevice
{

public:

    ALSADevice();
    virtual ~ALSADevice();
//    status_t init(alsa_device_t *module, ALSAHandleList &list);
    struct snd_ctl_card_info *getSoundCardInfo();
    status_t initCheck();
    status_t open(alsa_handle_t *handle);
    status_t close(alsa_handle_t *handle, uint32_t vsid = 0);
    status_t standby(alsa_handle_t *handle);
    status_t route(alsa_handle_t *handle, uint32_t devices, int mode);
    status_t startVoiceCall(alsa_handle_t *handle, uint32_t vsid = 0);
    status_t startVoipCall(alsa_handle_t *handle);
    status_t startFm(alsa_handle_t *handle);
    status_t startSpkProtRxTx(alsa_handle_t *handle, bool is_rx);
    bool     isSpeakerinUse(unsigned long &secs);
    status_t     setVocSessionId(uint32_t sessionId);
    void     setVoiceVolume(int volume);
    void     setVoipVolume(int volume);
    void     setMicMute(int state);
    void     setVoipMicMute(int state);
    void     setVoipConfig(int mode, int rate);
    void     setVoipEvrcMinMaxRate(int minRate, int maxRate);
    void     enableVoipDtx(bool flag);
    status_t setFmVolume(int vol);
    void     setBtscoRate(int rate);
    status_t setLpaVolume(alsa_handle_t *handle, int vol);
    void     enableWideVoice(bool flag, uint32_t vsid = 0);
    void     enableFENS(bool flag, uint32_t vsid = 0);
    void     setFlags(uint32_t flag);
    status_t setCompressedVolume(alsa_handle_t *handle, int vol);
    status_t setChannelMap(alsa_handle_t *handle, int maxChannels);
    void     enableSlowTalk(bool flag, uint32_t vsid = 0);
    status_t setDMID();
#ifdef QCOM_DS1_DOLBY_DAP
    status_t setEndpDevice(int value);
#endif
    void     setVocRecMode(uint8_t mode);
    void     setVoLTEMicMute(int state);
    void     setVoLTEVolume(int vol);
    void     setVoice2MicMute(int state);
    void     setVoice2Volume(int vol);
    status_t setEcrxDevice(char *device);
    void     setInChannels(int);
    //TODO:check if this needs to be public
    void     disableDevice(alsa_handle_t *handle);
    char    *getUCMDeviceFromAcdbId(int acdb_id);
    status_t getEDIDData(char *hdmiEDIDData);
    status_t updateDDPEndpTable(int device, int dev_ch_cap,
                                int param_id, int param_val);
    status_t setDDPEndpParams(alsa_handle_t *handle, int device, int dev_ch_cap,
                               char *ddpEndpParams, int *length, bool send_params);
    status_t getRMS(int *valp);
    void setCustomStereoOnOff(bool flag);
#ifdef SEPERATED_AUDIO_INPUT
    void     setInput(int);
#endif
#ifdef QCOM_CSDCLIENT_ENABLED
    void     setCsdHandle(void*);
#endif
#ifdef QCOM_ACDB_ENABLED
    void     setACDBHandle(void*);
    int getTxACDBID();
    int getRxACDBID();
#endif
    void     setSpkrProtHandle(AudioSpeakerProtection*);

    int mADSPState;
    int mCurDevice;
public:
#ifdef QCOM_WFD_ENABLED
    status_t setProxyPortChannelCount(int channels);
    int getWFDChannelCaps();
    void setWFDChannelCaps(int);
#endif
protected:
    friend class AudioHardwareALSA;
    friend class AudioSpeakerProtection;
private:
    void     switchDevice(alsa_handle_t *handle, uint32_t devices, uint32_t mode);
    int      getUseCaseType(const char *useCase);
    status_t setHDMIChannelCount();
    void     setChannelAlloc(int channelAlloc);
    status_t setHardwareParams(alsa_handle_t *handle);
    int      deviceName(alsa_handle_t *handle, unsigned flags, char **value);
    status_t setSoftwareParams(alsa_handle_t *handle);
    status_t getMixerControl(const char *name, unsigned int &value, int index = 0);
    status_t getMixerControlExt(const char *name, unsigned **getValues, unsigned *count);
    status_t setMixerControl(const char *name, unsigned int value, int index = -1);
    status_t setMixerControl(const char *name, const char *);
    status_t setMixerControlExt(const char *name, int count, char **setValues);
    char *   getUCMDevice(uint32_t devices, int input, char *rxDevice);
    status_t  start(alsa_handle_t *handle);

    status_t   openProxyDevice();
    status_t   closeProxyDevice();
    bool       isProxyDeviceOpened();
    bool       isProxyDeviceSuspended();
    bool       suspendProxy();
    bool       resumeProxy();
    void       resetProxyVariables();
    ssize_t    readFromProxy(void **captureBuffer , ssize_t *bufferSize);
    status_t   exitReadFromProxy();
    void       initProxyParams();
    status_t   startProxy();
    void       spkrCalibStatusUpdate(bool);
private:
    char mMicType[25];
    char mCurRxUCMDevice[50];
    char mCurTxUCMDevice[50];
    int mTxACDBID;
    int mRxACDBID;
    //fluence mode value: FLUENCE_MODE_BROADSIDE or FLUENCE_MODE_ENDFIRE
    uint32_t mFluenceMode;
    int mFmVolume;
    uint32_t mDevSettingsFlag;
    int mBtscoSamplerate;
    ALSAUseCaseList mUseCaseList;
    void *mcsd_handle;
    void *macdb_handle;
    int mCallMode;
    struct mixer*  mMixer;
    int mInChannels;
    int mDevChannelCap;
    bool mIsSglte;
    bool mIsFmEnabled;
#ifdef SEPERATED_AUDIO_INPUT
    int mInput_source;
#endif
#ifdef QCOM_WFD_ENABLED
    int mWFDChannelCap;
#endif

    struct snd_ctl_card_info mSndCardInfo;
    status_t mStatus;

//   ALSAHandleList  *mDeviceList;

    struct proxy_params {
        bool                mExitRead;
        struct pcm          *mProxyPcmHandle;
        uint32_t            mCaptureBufferSize;
        void                *mCaptureBuffer;
        enum {
            EProxyClosed    = 0,
            EProxyOpened    = 1,
            EProxySuspended = 2,
            EProxyCapture   = 3,
        };

        uint32_t mProxyState;
        struct snd_xferi mX;
        unsigned mAvail;
        struct pollfd mPfdProxy[NUM_FDS];
        long mFrames;
        long mBufferTime;
    };
    struct proxy_params mProxyParams;
    bool mSpkrInUse;
    bool mSpkrCalibrationDone;
    struct timeval mSpkLastUsedTime;
    AudioSpeakerProtection *mSpkrProt;
};

// ----------------------------------------------------------------------------

class ALSAMixer
{
public:
    ALSAMixer();
    virtual                ~ALSAMixer();

    bool                    isValid() { return 1;}
    status_t                setMasterVolume(float volume);
    status_t                setMasterGain(float gain);

    status_t                setVolume(uint32_t device, float left, float right);
    status_t                setGain(uint32_t device, float gain);

    status_t                setCaptureMuteState(uint32_t device, bool state);
    status_t                getCaptureMuteState(uint32_t device, bool *state);
    status_t                setPlaybackMuteState(uint32_t device, bool state);
    status_t                getPlaybackMuteState(uint32_t device, bool *state);

};

class ALSAStreamOps
{
public:
    ALSAStreamOps(AudioHardwareALSA *parent, alsa_handle_t *handle);
    virtual            ~ALSAStreamOps();

    status_t            set(int *format, uint32_t *channels, uint32_t *rate, uint32_t device);

    status_t            setParameters(const String8& keyValuePairs);
    String8             getParameters(const String8& keys);

    uint32_t            sampleRate() const;
    size_t              bufferSize() const;
    int                 format() const;
    uint32_t            channels() const;

    status_t            open(int mode);
    void                close();

private:
    bool checkTunnelCaptureMode(uint32_t &tunnelBufferSize) const;

protected:
    friend class AudioHardwareALSA;

    AudioHardwareALSA *     mParent;
    alsa_handle_t *         mHandle;
    uint32_t                mDevices;
};

// ----------------------------------------------------------------------------

class AudioStreamOutALSA : public AudioStreamOut, public ALSAStreamOps
{
public:
    AudioStreamOutALSA(AudioHardwareALSA *parent, alsa_handle_t *handle);
    virtual            ~AudioStreamOutALSA();

    virtual uint32_t    sampleRate() const
    {
        return ALSAStreamOps::sampleRate();
    }

    virtual size_t      bufferSize() const
    {
        return ALSAStreamOps::bufferSize();
    }

    virtual uint32_t    channels() const;

    virtual int         format() const
    {
        return ALSAStreamOps::format();
    }

    virtual uint32_t    latency() const;

    virtual ssize_t     write(const void *buffer, size_t bytes);
    virtual status_t    dump(int fd, const Vector<String16>& args);

    status_t            setVolume(float left, float right);

    virtual status_t    standby();

    virtual status_t    setParameters(const String8& keyValuePairs) {
        return ALSAStreamOps::setParameters(keyValuePairs);
    }

    virtual String8     getParameters(const String8& keys) {
        return ALSAStreamOps::getParameters(keys);
    }

    // return the number of audio frames written by the audio dsp to DAC since
    // the output has exited standby
    virtual status_t    getRenderPosition(uint32_t *dspFrames);

    status_t            open(int mode);
    status_t            close();

private:
    uint32_t            mFrameCount;
    uint32_t            mUseCase;

protected:
    AudioHardwareALSA *     mParent;
};

// ----------------------------------------------------------------------------
#ifdef QCOM_TUNNEL_LPA_ENABLED
class AudioSessionOutALSA : public AudioStreamOut
{
public:
    AudioSessionOutALSA(AudioHardwareALSA *parent,
                        uint32_t   devices,
                        int        format,
                        uint32_t   channels,
                        uint32_t   samplingRate,
                        int        type,
                        status_t   *status);
    virtual            ~AudioSessionOutALSA();

    virtual uint32_t    sampleRate() const
    {
        return mSampleRate;
    }

    virtual size_t      bufferSize() const
    {
        return mBufferSize;
    }

    virtual uint32_t    channels() const
    {
        return mChannels;
    }

    virtual int         format() const
    {
        return mFormat;
    }

    virtual uint32_t    latency() const;

    virtual ssize_t     write(const void *buffer, size_t bytes);

    virtual status_t    start();
    virtual status_t    pause();
    virtual status_t    flush();
    virtual status_t    stop();

    virtual status_t    dump(int fd, const Vector<String16>& args);

    status_t            setVolume(float left, float right);

    virtual status_t    standby();

    virtual status_t    setParameters(const String8& keyValuePairs);

    virtual String8     getParameters(const String8& keys);


    // return the number of audio frames written by the audio dsp to DAC since
    // the output has exited standby
    virtual status_t    getRenderPosition(uint32_t *dspFrames);

    virtual status_t    getNextWriteTimestamp(int64_t *timestamp);

    virtual status_t    setObserver(void *observer);

    virtual status_t    getBufferInfo(buf_info **buf);
    virtual status_t    isBufferAvailable(int *isAvail);
    status_t            pause_l();
    status_t            resume_l();

    void updateMetaData(size_t bytes);
    status_t setMetaDataMode();

private:
    Mutex               mLock;
    uint32_t            mFrameCount;
    uint32_t            mSampleRate;
    uint32_t            mChannels;
    size_t              mBufferSize;
    int                 mFormat;
    uint32_t            mStreamVol;

    bool                mPaused;
    bool                mSkipEOS;
    bool                mSeeking;
    bool                mReachedEOS;
    bool                mSkipWrite;
    bool                mEosEventReceived;
    int                 mSessionStatus;
    AudioHardwareALSA  *mParent;
    alsa_handle_t *     mAlsaHandle;
    ALSADevice *     mAlsaDevice;
    snd_use_case_mgr_t *mUcMgr;
    AudioEventObserver *mObserver;
    output_metadata_handle_t mOutputMetadataTunnel;
    uint32_t            mOutputMetadataLength;
    uint32_t            mUseCase;
    status_t            openDevice(char *pUseCase, bool bIsUseCase, int devices);

    status_t            closeDevice(alsa_handle_t *pDevice);
    void                createEventThread();
    void                bufferAlloc(alsa_handle_t *handle);
    void                bufferDeAlloc();
    bool                isReadyToPostEOS(int errPoll, void *fd);
    status_t            drain();
    status_t            openAudioSessionDevice(int type, int devices);
    // make sure the event thread also exited
    void                requestAndWaitForEventThreadExit();
    int32_t             writeToDriver(char *buffer, int bytes);
    static void *       eventThreadWrapper(void *me);
    void                eventThreadEntry();
    void                reset();
    status_t            drainAndPostEOS_l();

    //Structure to hold mem buffer information
    class BuffersAllocated {
    public:
        BuffersAllocated(void *buf1, int32_t nSize) :
        memBuf(buf1), memBufsize(nSize), bytesToWrite(0)
        {}
        void* memBuf;
        int32_t memBufsize;
        uint32_t bytesToWrite;
    };
    List<BuffersAllocated> mEmptyQueue;
    List<BuffersAllocated> mFilledQueue;
    List<BuffersAllocated> mBufPool;

    //Declare all the threads
    pthread_t mEventThread;

    //Declare the condition Variables and Mutex
    Mutex mEmptyQueueMutex;
    Mutex mFilledQueueMutex;

    //Mutex for sync between decoderthread and control thread
    Mutex mDecoderLock;

    Condition mWriteCv;
    Condition mEventCv;
    bool mKillEventThread;
    bool mEventThreadAlive;
    int mInputBufferSize;
    int mInputBufferCount;

    //event fd to signal the EOS and Kill from the userspace
    int mEfd;
    bool mTunnelMode;

public:
    bool mRouteAudioToA2dp;
};
#endif //QCOM_TUNNEL_LPA_ENABLED

class AudioStreamInALSA : public AudioStreamIn, public ALSAStreamOps
{
public:
    AudioStreamInALSA(AudioHardwareALSA *parent,
            alsa_handle_t *handle,
            AudioSystem::audio_in_acoustics audio_acoustics);
    virtual            ~AudioStreamInALSA();

    virtual uint32_t    sampleRate() const
    {
        return ALSAStreamOps::sampleRate();
    }

    virtual size_t      bufferSize() const
    {
        return ALSAStreamOps::bufferSize();
    }

    virtual uint32_t    channels() const
    {
        return ALSAStreamOps::channels();
    }

    virtual int         format() const
    {
        return ALSAStreamOps::format();
    }

    virtual ssize_t     read(void* buffer, ssize_t bytes);
    virtual status_t    dump(int fd, const Vector<String16>& args);

    virtual status_t    setGain(float gain);

    virtual status_t    standby();

    virtual status_t    setParameters(const String8& keyValuePairs)
    {
        return ALSAStreamOps::setParameters(keyValuePairs);
    }

    virtual String8     getParameters(const String8& keys)
    {
        return ALSAStreamOps::getParameters(keys);
    }

    // Return the amount of input frames lost in the audio driver since the last call of this function.
    // Audio driver is expected to reset the value to 0 and restart counting upon returning the current value by this function call.
    // Such loss typically occurs when the user space process is blocked longer than the capacity of audio driver buffers.
    // Unit: the number of input audio frames
    virtual unsigned int  getInputFramesLost() const;

    virtual status_t addAudioEffect(effect_handle_t effect)
    {
        return BAD_VALUE;
    }

    virtual status_t removeAudioEffect(effect_handle_t effect)
    {
        return BAD_VALUE;
    }
    status_t            setAcousticParams(void* params);

    status_t            open(int mode);
    status_t            close();
#ifdef QCOM_SSR_ENABLED
    // Helper function to initialize the Surround Sound library.
    status_t initSurroundSoundLibrary(unsigned long buffersize);
#endif

private:
    void                resetFramesLost();

    unsigned int        mFramesLost;
    AudioSystem::audio_in_acoustics mAcoustics;

#ifdef QCOM_SSR_ENABLED
    // Function to read coefficients from files.
    status_t            readCoeffsFromFile();

    FILE                *mFp_4ch;
    FILE                *mFp_6ch;
    int16_t             **mRealCoeffs;
    int16_t             **mImagCoeffs;
    void                *mSurroundObj;

    int16_t             *mSurroundInputBuffer;
    int16_t             *mSurroundOutputBuffer;
    int                 mSurroundInputBufferIdx;
    int                 mSurroundOutputBufferIdx;
#endif

    uint8_t             *mAmrwbInputBuffer;
protected:
    AudioHardwareALSA *     mParent;
};

class AudioSpeakerProtection {
public:
    /*Feedback Speaker Protection functions*/
    void startSpkrProcessing();
    void stopSpkrProcessing();
    void initialize(void  *);
    void cancelCalibration();
    AudioSpeakerProtection();
    ~AudioSpeakerProtection();
    void updateSpkrT0(int t0);
private:
    int spkCalibrate(int);
    static void* spkrCalibrationThread(void *context);
    int          mSpkrProtMode;
    int          mSpkrProcessingState;
    Mutex        mMutexSpkrProt;
    pthread_t    mSpkrCalibrationThread;
    int          mSpkrProtT0;
    Mutex        mSpkrProtThermalSyncMutex;
    Condition    mSpkrProtThermalSync;
    int          mCancelSpkrCalib;
    Condition    mSpkrCalibCancel;
    Mutex        mSpkrCalibCancelAckMutex;
    Condition    mSpkrCalibCancelAck;
    ALSADevice*  mALSADevice;
    pthread_t    mSpeakerProtthreadid;
    void *mThermalHandle;
    int   mThermalClientHandle;
    void  *mAcdbHandle;
    snd_use_case_mgr_t *mUcMgr;
    AudioHardwareALSA *mParent;
};

class AudioHardwareALSA : public AudioHardwareBase
{
public:
    AudioHardwareALSA();
    virtual            ~AudioHardwareALSA();

    /**
     * check to see if the audio hardware interface has been initialized.
     * return status based on values defined in include/utils/Errors.h
     */
    virtual status_t    initCheck();

    /** set the audio volume of a voice call. Range is between 0.0 and 1.0 */
    virtual status_t    setVoiceVolume(float volume);

    /**
     * set the audio volume for all audio activities other than voice call.
     * Range between 0.0 and 1.0. If any value other than NO_ERROR is returned,
     * the software mixer will emulate this capability.
     */
    virtual status_t    setMasterVolume(float volume);
#ifdef QCOM_FM_ENABLED
#ifndef QCOM_FM_V2_ENABLED
    virtual status_t    setFmVolume(float volume);
#endif
#endif
    /**
     * setMode is called when the audio mode changes. NORMAL mode is for
     * standard audio playback, RINGTONE when a ringtone is playing, and IN_CALL
     * when a call is in progress.
     */
    virtual status_t    setMode(int mode);

    // mic mute
    virtual status_t    setMicMute(bool state);
    virtual status_t    getMicMute(bool* state);

    // set/get global audio parameters
    virtual status_t    setParameters(const String8& keyValuePairs);
    virtual String8     getParameters(const String8& keys);

    // Returns audio input buffer size according to parameters passed or 0 if one of the
    // parameters is not supported
    virtual size_t    getInputBufferSize(uint32_t sampleRate, int format, int channels);

#ifdef QCOM_TUNNEL_LPA_ENABLED
    /** This method creates and opens the audio hardware output
      *  session for LPA */
    virtual AudioStreamOut* openOutputSession(
            uint32_t devices,
            int *format,
            status_t *status,
            int sessionId,
            uint32_t samplingRate=0,
            uint32_t channels=0);
    virtual void closeOutputSession(AudioStreamOut* out);
#endif

    /** This method creates and opens the audio hardware output stream */
    virtual AudioStreamOut* openOutputStream(
            uint32_t devices,
            int *format=0,
            uint32_t *channels=0,
            uint32_t *sampleRate=0,
            status_t *status=0);
    virtual    void        closeOutputStream(AudioStreamOut* out);

    /** This method creates and opens the audio hardware input stream */
    virtual AudioStreamIn* openInputStream(
            uint32_t devices,
            int *format,
            uint32_t *channels,
            uint32_t *sampleRate,
            status_t *status,
            AudioSystem::audio_in_acoustics acoustics);
    virtual    void        closeInputStream(AudioStreamIn* in);

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    status_t openListenSession(ListenSession** handle);
    status_t closeListenSession(ListenSession* handle);
    status_t setMadObserver(listen_callback_t cb_func);
#endif

    status_t    startPlaybackOnExtOut(uint32_t activeUsecase);
    status_t    stopPlaybackOnExtOut(uint32_t activeUsecase);
    status_t    setProxyProperty(uint32_t value);
    bool        suspendPlaybackOnExtOut(uint32_t activeUsecase);

    status_t    startPlaybackOnExtOut_l(uint32_t activeUsecase);
    status_t    stopPlaybackOnExtOut_l(uint32_t activeUsecase);
    bool        suspendPlaybackOnExtOut_l(uint32_t activeUsecase);
    status_t    isExtOutDevice(int device);
    /**This method dumps the state of the audio hardware */
    //virtual status_t dumpState(int fd, const Vector<String16>& args);

    static AudioHardwareInterface* create();

    int                 mode()
    {
        return mMode;
    }
    void pauseIfUseCaseTunnelOrLPA();
    void resumeIfUseCaseTunnelOrLPA();
    uint32_t getActiveSessionVSID();
private:
    AudioSpeakerProtection mspkrProtection;
    status_t     openExtOutput(int device);
    status_t     closeExtOutput(int device);
    status_t     openA2dpOutput();
    status_t     closeA2dpOutput();
    status_t     openUsbOutput();
    status_t     closeUsbOutput();
    status_t     stopExtOutThread();
    void         extOutThreadFunc();
    static void* spkrCalibrationThread(void *context);
    static void* extOutThreadWrapper(void *context);
    void         setExtOutActiveUseCases_l(uint32_t activeUsecase);
    uint32_t     getExtOutActiveUseCases_l();
    void         clearExtOutActiveUseCases_l(uint32_t activeUsecase);
    uint32_t     useCaseStringToEnum(const char *usecase);
    void         switchExtOut(int device);
    status_t     setDDPEndpParams(int device);
    void         parseDDPParams(int ddp_dev, int ddp_ch_cap, AudioParameter *param);
    unsigned int mTunnelsUsed;
    char*        getTunnel(bool hifi);
    void         freeTunnel(char* useCase);
    int          getmCallState(uint32_t vsid, enum call_state state);
    bool         isAnyCallActive();
    int*         getCallStateForVSID(uint32_t vsid);
    char*        getUcmVerbForVSID(uint32_t vsid);
    char*        getUcmModForVSID(uint32_t vsid);
    alsa_handle_t* getALSADeviceHandleForVSID(uint32_t vsid);
#ifdef RESOURCE_MANAGER
    enum {
       CONCURRENCY_INACTIVE = 0,
       CONCURRENCY_ACTIVE,
    };
    status_t     handleFmConcurrency(int32_t device, uint32_t &state);
    status_t     setParameterForConcurrency(String8 useCase, uint32_t state);
#endif
protected:
    virtual status_t    dump(int fd, const Vector<String16>& args);
    virtual uint32_t    getVoipMode(int format);
    status_t            doRouting(int device, char* useCase);
#ifdef QCOM_FM_ENABLED
    void                handleFm(int device);
#endif
#ifdef QCOM_WFD_ENABLED
    // Copies mWFDChannelCap values to the arg
    void getWFDAudioSinkCaps(int32_t &, int32_t &);
#endif
#ifdef QCOM_USBAUDIO_ENABLED
    void                closeUSBPlayback();
    void                closeUSBRecording();
    void                closeUsbRecordingIfNothingActive();
    void                closeUsbPlaybackIfNothingActive();
    void                startUsbPlaybackIfNotStarted();
    void                startUsbRecordingIfNotStarted();
#endif
    void                setInChannels(int device);
    void                disableVoiceCall(int mode, int device, uint32_t vsid = 0);
    status_t            enableVoiceCall(int mode, int device, uint32_t vsid = 0);
    bool                routeCall(int device, int newMode, uint32_t vsid);
    friend class AudioSessionOutALSA;
    friend class AudioStreamOutALSA;
    friend class AudioStreamInALSA;
    friend class ALSAStreamOps;
    friend class AudioSpeakerProtection;

    ALSADevice*     mALSADevice;

    ALSAHandleList      mDeviceList;

#ifdef QCOM_USBAUDIO_ENABLED
    AudioUsbALSA        *mAudioUsbALSA;
#endif

    Mutex                   mLock;

    snd_use_case_mgr_t *mUcMgr;

    int32_t            mCurRxDevice;
    int32_t            mCurDevice;
    int32_t            mCanOpenProxy;
    /* The flag holds all the audio related device settings from
     * Settings and Qualcomm Settings applications */
    uint32_t            mDevSettingsFlag;
    uint32_t            mVoipInStreamCount;
    uint32_t            mVoipOutStreamCount;
    bool                mVoipMicMute;
    uint32_t            mVoipBitRate;
    uint32_t            mVoipEvrcBitRateMin;
    uint32_t            mVoipEvrcBitRateMax;
    uint32_t            mIncallMode;

    bool                mMicMute;
    int mVoiceCallState;
    int mVolteCallState;
    int mVoice2CallState;
    int mQchatCallState;
    int mCallState;
    uint32_t mVSID;
    int mVoiceVolFeatureSet;
    int mIsFmActive;
    bool mBluetoothVGS;
    bool mFusion3Platform;
#ifdef QCOM_USBAUDIO_ENABLED
    int musbPlaybackState;
    int musbRecordingState;
#endif

    void *mAcdbHandle;
    void *mCsdHandle;
    //fluence key value: fluencepro, fluence, or none
    char mFluenceKey[20];
    //A2DP variables
    audio_stream_out   *mA2dpStream;
    audio_hw_device_t  *mA2dpDevice;

    audio_stream_out   *mUsbStream;
    audio_hw_device_t  *mUsbDevice;
    audio_stream_out   *mExtOutStream;
    struct resampler_itfe *mResampler;


    volatile bool       mKillExtOutThread;
    volatile bool       mExtOutThreadAlive;
    pthread_t           mExtOutThread;
    Mutex               mExtOutMutex;
    Condition           mExtOutCv;
    volatile bool       mIsExtOutEnabled;

    enum {
      USECASE_NONE = 0x0,
      USECASE_HIFI = 0x1,
      USECASE_HIFI_LOWLATENCY = 0x2,
      USECASE_HIFI_LOW_POWER = 0x4,
      USECASE_HIFI_TUNNEL = 0x8,
      USECASE_FM = 0x10,
      USECASE_HIFI_TUNNEL2 = 0x20,
      USECASE_HIFI_TUNNEL3 = 0x40,
      USECASE_HIFI_TUNNEL4 = 0x80,
      USECASE_HIFI_INCALL_DELIVERY = 0x100,
      USECASE_HIFI_INCALL_DELIVERY2 = 0x200,
    };
    uint32_t mExtOutActiveUseCases;
    status_t mStatus;

#ifdef QCOM_LISTEN_FEATURE_ENABLE
    ListenHardware *mListenHw;
#endif
#ifdef RESOURCE_MANAGER
    AudioResourceManager *mAudioResourceManager;
#endif
public:
    bool mRouteAudioToExtOut;
};

static bool isTunnelUseCase(const char *useCase) {
    if (useCase == NULL) {
        ALOGE("isTunnelUseCase: invalid use case, return false");
        return false;
    }
    if ((!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                           MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
        (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2,
                           MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL2))) ||
        (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL3,
                           MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL3))) ||
        (!strncmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL4,
                           MAX_LEN(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL4))) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                           MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2,
                           MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL2))) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3,
                           MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL3))) ||
        (!strncmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL4,
                           MAX_LEN(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL4)))) {
        return true;
    }
    return false;
}
// ----------------------------------------------------------------------------

};        // namespace android_audio_legacy
#endif    // ANDROID_AUDIO_HARDWARE_ALSA_H
