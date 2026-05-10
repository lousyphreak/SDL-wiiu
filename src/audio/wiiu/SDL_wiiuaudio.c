/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_WIIU

#include "../SDL_audiodev_c.h"
#include "../SDL_sysaudio.h"
#include "../../core/wiiu/SDL_wiiuax.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"
#include "SDL_wiiuaudio.h"

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>

#define WIIUAUDIO_DRIVER_NAME "wiiu"
#define WIIUAUDIO_PLAYBACK_NAME "Wii U system audio"
#define WIIUAUDIO_RECORDING_NAME "Wii U microphone"
#define WIIUAUDIO_VOICE_PRIORITY 31
#define WIIUAUDIO_MAX_GAIN 0x8000

static const int WIIUAUDIO_TV_5POINT1_CHMAP[WIIU_AUDIO_MAX_TV_CHANNELS] = { 0, 1, 4, 5, 2, 3 };

static Uint32 WIIUAUDIO_AlignFrames(Uint32 frames, Uint32 quantum)
{
    if ((frames == 0) || (quantum == 0)) {
        return quantum ? quantum : 1;
    }

    return ((frames + quantum - 1) / quantum) * quantum;
}

static Uint32 WIIUAUDIO_DistanceFrames(Uint32 from, Uint32 to, Uint32 ring_frames)
{
    return (to >= from) ? (to - from) : (ring_frames - from + to);
}

static Uint16 WIIUAUDIO_FloatToGain(float gain)
{
    const float clamped = SDL_clamp(gain, 0.0f, 1.0f);
    return (Uint16)(clamped * (float)WIIUAUDIO_MAX_GAIN);
}

static Uint32 WIIUAUDIO_FallbackChannelCount(AXDeviceType type, AXDeviceMode mode)
{
    switch (mode) {
    case AX_DEVICE_MODE_MONO:
        return 1;
    case AX_DEVICE_MODE_6_CHANNEL:
        return (type == AX_DEVICE_TYPE_TV) ? 6u : 2u;
    case AX_DEVICE_MODE_STEREO:
    case AX_DEVICE_MODE_SURROUND:
    case AX_DEVICE_MODE_DPL2:
    default:
        return 2;
    }
}

static Uint32 WIIUAUDIO_QueryChannelCount(AXDeviceType type, Uint32 fallback)
{
    Uint32 count = 0;

    if ((AXGetDeviceChannelCount(type, 0, &count) == AX_RESULT_SUCCESS) && (count > 0)) {
        return count;
    }

    {
        AXDeviceMode mode = AX_DEVICE_MODE_STEREO;
        if (AXGetDeviceMode(type, &mode) == AX_RESULT_SUCCESS) {
            return WIIUAUDIO_FallbackChannelCount(type, mode);
        }
    }

    return fallback;
}

static Uint32 WIIUAUDIO_GetPreferredPlaybackChannels(Uint32 requested_channels, Uint32 tv_channels)
{
    if ((tv_channels >= WIIU_AUDIO_MAX_TV_CHANNELS) && (requested_channels > 2)) {
        return WIIU_AUDIO_MAX_TV_CHANNELS;
    }

    return 2;
}

static bool WIIUAUDIO_SetDeviceChannelMap(SDL_AudioDevice *device)
{
    SDL_free(device->chmap);
    device->chmap = NULL;

    if (device->spec.channels == WIIU_AUDIO_MAX_TV_CHANNELS) {
        device->chmap = SDL_ChannelMapDup(WIIUAUDIO_TV_5POINT1_CHMAP, WIIU_AUDIO_MAX_TV_CHANNELS);
        if (!device->chmap) {
            return false;
        }
    }

    return true;
}

static void WIIUAUDIO_InitTVChannelMix(SDL_AudioDevice *device, Uint32 channel, AXVoiceDeviceMixData *mix)
{
    SDL_memset(mix, 0, sizeof(*mix) * WIIU_AUDIO_MAX_TV_CHANNELS);

    if (device->hidden->tv_channels == 1) {
        switch (channel) {
        case 0:
        case 1:
            mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
            break;
        default:
            break;
        }
    } else if (channel < device->hidden->tv_channels) {
        mix[channel].bus[0].volume = WIIUAUDIO_MAX_GAIN;
    }
}

static void WIIUAUDIO_InitDRCChannelMix(SDL_AudioDevice *device, Uint32 channel, AXVoiceDeviceMixData *mix)
{
    const Uint32 drc_channels = device->hidden->drc_channels;

    SDL_memset(mix, 0, sizeof(*mix) * WIIU_AUDIO_MAX_DRC_CHANNELS);

    if (drc_channels == 0) {
        return;
    }

    if (drc_channels == 1) {
        switch (channel) {
        case 0:
        case 1:
            mix[0].bus[0].volume = WIIUAUDIO_MAX_GAIN;
            break;
        case 2:
        case 3:
            mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
            break;
        case 4:
            mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
            break;
        case 5:
            mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.5f);
            break;
        default:
            break;
        }
        return;
    }

    switch (channel) {
    case 0:
        mix[0].bus[0].volume = WIIUAUDIO_MAX_GAIN;
        break;
    case 1:
        mix[1].bus[0].volume = WIIUAUDIO_MAX_GAIN;
        break;
    case 2:
        mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
        break;
    case 3:
        mix[1].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
        break;
    case 4:
        mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
        mix[1].bus[0].volume = WIIUAUDIO_FloatToGain(0.70710678f);
        break;
    case 5:
        mix[0].bus[0].volume = WIIUAUDIO_FloatToGain(0.5f);
        mix[1].bus[0].volume = WIIUAUDIO_FloatToGain(0.5f);
        break;
    default:
        break;
    }
}

static const char *WIIUAUDIO_MICErrorName(MICError error)
{
    switch (error) {
    case MIC_ERROR_OK: return "MIC_ERROR_OK";
    case MIC_ERROR_NOT_SUPPORTED: return "MIC_ERROR_NOT_SUPPORTED";
    case MIC_ERROR_INVALID_ARG: return "MIC_ERROR_INVALID_ARG";
    case MIC_ERROR_INVALID_STATE: return "MIC_ERROR_INVALID_STATE";
    case MIC_ERROR_NO_MEMORY: return "MIC_ERROR_NO_MEMORY";
    case MIC_ERROR_ALREADY_OPEN: return "MIC_ERROR_ALREADY_OPEN";
    case MIC_ERROR_NOT_OPEN: return "MIC_ERROR_NOT_OPEN";
    case MIC_ERROR_NOT_INITIALIZED: return "MIC_ERROR_NOT_INITIALIZED";
    case MIC_ERROR_NOT_CONNECTED: return "MIC_ERROR_NOT_CONNECTED";
    default: return "MIC_ERROR_UNKNOWN";
    }
}

static bool WIIUAUDIO_BeginVoice(AXVoice *voice)
{
    return AXVoiceBegin(voice) >= 0;
}

static void WIIUAUDIO_EndVoice(AXVoice *voice)
{
    AXVoiceEnd(voice);
}

static bool WIIUAUDIO_ConfigureVoice(SDL_AudioDevice *device, AXVoice *voice, Sint16 *samples, Uint32 ring_frames, Uint32 channel)
{
    AXVoiceOffsets offsets;
    AXVoiceVeData ve;
    AXVoiceDeviceMixData tv_mix[WIIU_AUDIO_MAX_TV_CHANNELS];
    AXVoiceDeviceMixData drc_mix[WIIU_AUDIO_MAX_DRC_CHANNELS];

    SDL_zero(offsets);
    SDL_zero(ve);

    offsets.dataType = AX_VOICE_FORMAT_LPCM16;
    offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
    offsets.loopOffset = 0;
    offsets.endOffset = ring_frames;
    offsets.currentOffset = 0;
    offsets.data = samples;

    ve.volume = WIIUAUDIO_MAX_GAIN;
    WIIUAUDIO_InitTVChannelMix(device, channel, tv_mix);
    WIIUAUDIO_InitDRCChannelMix(device, channel, drc_mix);

    if (!WIIUAUDIO_BeginVoice(voice)) {
        return SDL_SetError("AXVoiceBegin failed for Wii U playback channel %u", (unsigned int)channel);
    }

    AXSetVoiceType(voice, AX_VOICE_TYPE_STREAM);
    AXSetVoiceSrcType(voice, AX_VOICE_SRC_TYPE_NONE);
    AXSetVoiceSrcRatio(voice, 1.0f);
    AXSetVoiceOffsets(voice, &offsets);
    AXSetVoiceVe(voice, &ve);

    if (AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_TV, 0, tv_mix) != AX_RESULT_SUCCESS) {
        WIIUAUDIO_EndVoice(voice);
        return SDL_SetError("AXSetVoiceDeviceMix failed for Wii U TV output channel %u", (unsigned int)channel);
    }

    if (AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_DRC, 0, drc_mix) != AX_RESULT_SUCCESS) {
        WIIUAUDIO_EndVoice(voice);
        return SDL_SetError("AXSetVoiceDeviceMix failed for Wii U DRC output channel %u", (unsigned int)channel);
    }

    AXSetVoiceState(voice, AX_VOICE_STATE_STOPPED);
    WIIUAUDIO_EndVoice(voice);
    return true;
}

static void WIIUAUDIO_SetPlaybackState(SDL_AudioDevice *device, AXVoiceState state)
{
    for (Uint32 i = 0; i < device->hidden->playback_channels; ++i) {
        AXVoice *voice = device->hidden->voices[i];
        if (voice && WIIUAUDIO_BeginVoice(voice)) {
            AXSetVoiceState(voice, state);
            WIIUAUDIO_EndVoice(voice);
        }
    }
}

static bool WIIUAUDIO_ClearPlaybackRing(SDL_AudioDevice *device)
{
    const Uint32 ring_bytes = device->hidden->ring_frames * sizeof(Sint16);

    for (Uint32 i = 0; i < device->hidden->playback_channels; ++i) {
        SDL_memset(device->hidden->playback_channel_buffers[i], 0, ring_bytes);
        DCFlushRange(device->hidden->playback_channel_buffers[i], ring_bytes);
        if (device->hidden->voices[i]) {
            if (!WIIUAUDIO_ConfigureVoice(device,
                                          device->hidden->voices[i],
                                          device->hidden->playback_channel_buffers[i],
                                          device->hidden->ring_frames,
                                          i)) {
                return false;
            }
        }
    }

    device->hidden->write_frame_offset = (Uint32)device->sample_frames;
    return true;
}

static bool WIIUAUDIO_UpdateForegroundState(SDL_AudioDevice *device)
{
    const bool in_foreground = SDL_WIIU_IsInForeground();

    if (device->hidden->foreground_active == in_foreground) {
        return true;
    }

    device->hidden->foreground_active = in_foreground;
    if (in_foreground) {
        if (!WIIUAUDIO_ClearPlaybackRing(device)) {
            return false;
        }
        WIIUAUDIO_SetPlaybackState(device, AX_VOICE_STATE_PLAYING);
    } else {
        WIIUAUDIO_SetPlaybackState(device, AX_VOICE_STATE_STOPPED);
    }

    return true;
}

static Uint32 WIIUAUDIO_GetPlaybackReadOffset(SDL_AudioDevice *device)
{
    Uint32 offset = AXGetVoiceCurrentOffsetEx(device->hidden->voices[0], device->hidden->playback_channel_buffers[0]);
    if (device->hidden->ring_frames > 0) {
        offset %= device->hidden->ring_frames;
    }
    return offset;
}

static void WIIUAUDIO_ClosePlaybackDevice(SDL_AudioDevice *device)
{
    if (!device->hidden) {
        return;
    }

    WIIUAUDIO_SetPlaybackState(device, AX_VOICE_STATE_STOPPED);

    for (int i = 0; i < WIIU_AUDIO_MAX_PLAYBACK_CHANNELS; ++i) {
        if (device->hidden->voices[i]) {
            AXFreeVoice(device->hidden->voices[i]);
            device->hidden->voices[i] = NULL;
        }
    }

    if (device->hidden->rawbuf) {
        SDL_aligned_free(device->hidden->rawbuf);
        device->hidden->rawbuf = NULL;
    }

    for (int i = 0; i < WIIU_AUDIO_MAX_PLAYBACK_CHANNELS; ++i) {
        if (device->hidden->playback_channel_buffers[i]) {
            SDL_aligned_free(device->hidden->playback_channel_buffers[i]);
            device->hidden->playback_channel_buffers[i] = NULL;
        }
    }

    SDL_WIIU_AX_Release();
}

static bool WIIUAUDIO_OpenPlaybackDevice(SDL_AudioDevice *device)
{
    const SDL_AudioFormat *closefmts = SDL_ClosestAudioFormats(device->spec.format);
    SDL_AudioFormat test_format = 0;
    Uint32 mixlen;
    Uint32 runtime_sample_rate;
    Uint32 runtime_frame_samples;
    Uint32 tv_channels;
    Uint32 ring_bytes;

    while ((test_format = *(closefmts++)) != 0) {
        if (test_format == SDL_AUDIO_S16) {
            device->spec.format = test_format;
            break;
        }
    }

    if (!test_format) {
        return SDL_SetError("Wii U audio backend requires native-endian signed 16-bit PCM");
    }

    if (!SDL_WIIU_AX_Acquire(&runtime_sample_rate, &runtime_frame_samples)) {
        return false;
    }

    tv_channels = WIIUAUDIO_QueryChannelCount(AX_DEVICE_TYPE_TV, 2);
    device->hidden->tv_channels = SDL_min(tv_channels, (Uint32)WIIU_AUDIO_MAX_TV_CHANNELS);
    device->hidden->drc_channels = SDL_min(WIIUAUDIO_QueryChannelCount(AX_DEVICE_TYPE_DRC, 2),
                                           (Uint32)WIIU_AUDIO_MAX_DRC_CHANNELS);
    device->hidden->playback_channels =
        WIIUAUDIO_GetPreferredPlaybackChannels((Uint32)device->spec.channels, device->hidden->tv_channels);
    device->spec.channels = (Uint8)device->hidden->playback_channels;

    if (!WIIUAUDIO_SetDeviceChannelMap(device)) {
        return false;
    }

    device->hidden->sample_rate = runtime_sample_rate;
    device->hidden->frame_samples = runtime_frame_samples;
    device->hidden->frame_duration_ns = ((Uint64)runtime_frame_samples * SDL_NS_PER_SECOND) / runtime_sample_rate;
    device->spec.freq = (int)runtime_sample_rate;
    device->sample_frames = (int)WIIUAUDIO_AlignFrames((Uint32)device->sample_frames, runtime_frame_samples);
    device->hidden->ring_frames = (Uint32)device->sample_frames * WIIU_AUDIO_RING_BUFFERS;

    SDL_UpdatedAudioDeviceFormat(device);

    mixlen = (Uint32)device->buffer_size * WIIU_AUDIO_MIX_BUFFERS;
    device->hidden->rawbuf = (Uint8 *)SDL_aligned_alloc(64, mixlen);
    if (!device->hidden->rawbuf) {
        return SDL_OutOfMemory();
    }

    SDL_memset(device->hidden->rawbuf, device->silence_value, mixlen);
    for (int i = 0; i < WIIU_AUDIO_MIX_BUFFERS; ++i) {
        device->hidden->mixbufs[i] = device->hidden->rawbuf + (i * device->buffer_size);
    }

    ring_bytes = device->hidden->ring_frames * sizeof(Sint16);
    for (Uint32 i = 0; i < device->hidden->playback_channels; ++i) {
        device->hidden->playback_channel_buffers[i] = (Sint16 *)SDL_aligned_alloc(64, ring_bytes);
        if (!device->hidden->playback_channel_buffers[i]) {
            return SDL_OutOfMemory();
        }
    }

    for (Uint32 i = 0; i < device->hidden->playback_channels; ++i) {
        device->hidden->voices[i] = AXAcquireVoice(WIIUAUDIO_VOICE_PRIORITY, NULL, NULL);
        if (!device->hidden->voices[i]) {
            return SDL_SetError("AXAcquireVoice failed for Wii U playback channel %u", (unsigned int)i);
        }
    }

    device->hidden->foreground_active = true;
    if (!WIIUAUDIO_ClearPlaybackRing(device)) {
        return false;
    }
    device->hidden->foreground_active = SDL_WIIU_IsInForeground();
    WIIUAUDIO_SetPlaybackState(device,
                               device->hidden->foreground_active ? AX_VOICE_STATE_PLAYING : AX_VOICE_STATE_STOPPED);
    return true;
}

static bool WIIUAUDIO_OpenRecordingDevice(SDL_AudioDevice *device)
{
    const SDL_AudioFormat *closefmts = SDL_ClosestAudioFormats(device->spec.format);
    SDL_AudioFormat test_format = 0;
    MICError mic_error = MIC_ERROR_OK;
    MICError status_error;
    uint32_t mic_rate = 0;
    const Uint32 initial_ring_count = SDL_max((Uint32)0x2800,
                                              (Uint32)SDL_GetDefaultSampleFramesFromFreq(32000) * 8u);

    while ((test_format = *(closefmts++)) != 0) {
        if (test_format == SDL_AUDIO_S16) {
            device->spec.format = test_format;
            break;
        }
    }

    if (!test_format) {
        return SDL_SetError("Wii U microphone backend requires native-endian signed 16-bit PCM");
    }

    device->spec.channels = 1;

    device->hidden->mic_sample_count = initial_ring_count;
    device->hidden->mic_sample_buffer =
        (Sint16 *)MEMAllocFromDefaultHeapEx(device->hidden->mic_sample_count * sizeof(Sint16), 0x40);
    if (!device->hidden->mic_sample_buffer) {
        return SDL_OutOfMemory();
    }

    SDL_memset(device->hidden->mic_sample_buffer, 0, device->hidden->mic_sample_count * sizeof(Sint16));
    DCFlushRange(device->hidden->mic_sample_buffer, device->hidden->mic_sample_count * sizeof(Sint16));

    SDL_zero(device->hidden->mic_resource);
    device->hidden->mic_ring.sampleMaxCount = device->hidden->mic_sample_count;
    device->hidden->mic_ring.sampleBuffer = device->hidden->mic_sample_buffer;

    device->hidden->mic_handle = MICInit(MIC_INSTANCE_0,
                                         &device->hidden->mic_resource,
                                         &device->hidden->mic_ring,
                                         &mic_error);
    if (device->hidden->mic_handle < 0) {
        return SDL_SetError("MICInit failed: %s", WIIUAUDIO_MICErrorName(mic_error));
    }

    status_error = MICOpen(device->hidden->mic_handle);
    if (status_error != MIC_ERROR_OK) {
        return SDL_SetError("MICOpen failed: %s", WIIUAUDIO_MICErrorName(status_error));
    }

    status_error = MICGetState(device->hidden->mic_handle, MIC_STATE_SAMPLE_RATE, &mic_rate);
    if (status_error != MIC_ERROR_OK) {
        return SDL_SetError("MICGetState(sample rate) failed: %s", WIIUAUDIO_MICErrorName(status_error));
    }

    device->spec.freq = (int)mic_rate;
    device->sample_frames = SDL_GetDefaultSampleFramesFromFreq(device->spec.freq);
    SDL_UpdatedAudioDeviceFormat(device);
    return true;
}

static bool WIIUAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }

    device->hidden->mic_handle = -1;

    if (device->recording) {
        return WIIUAUDIO_OpenRecordingDevice(device);
    }
    return WIIUAUDIO_OpenPlaybackDevice(device);
}

static bool WIIUAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    const Uint32 max_queued_frames = device->hidden->ring_frames - (Uint32)device->sample_frames;

    while (!SDL_GetAtomicInt(&device->shutdown)) {
        if (!WIIUAUDIO_UpdateForegroundState(device)) {
            return false;
        }

        if (!device->hidden->foreground_active) {
            SDL_Delay(16);
            continue;
        }

        {
            const Uint32 read_offset = WIIUAUDIO_GetPlaybackReadOffset(device);
            const Uint32 queued_frames =
                WIIUAUDIO_DistanceFrames(read_offset, device->hidden->write_frame_offset, device->hidden->ring_frames);
            if (queued_frames <= (max_queued_frames - (Uint32)device->sample_frames)) {
                return true;
            }
        }

        SDL_DelayPrecise(device->hidden->frame_duration_ns);
    }

    return true;
}

static Uint8 *WIIUAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    Uint8 *result = device->hidden->mixbufs[device->hidden->next_buffer];
    device->hidden->next_buffer = (device->hidden->next_buffer + 1) % WIIU_AUDIO_MIX_BUFFERS;
    *buffer_size = device->buffer_size;
    return result;
}

static bool WIIUAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    const Sint16 *src = (const Sint16 *)buffer;
    const Uint32 frame_offset = device->hidden->write_frame_offset;
    const Uint32 frame_count = (Uint32)device->sample_frames;
    const Uint32 slot_bytes = frame_count * sizeof(Sint16);

    SDL_assert(buflen == device->buffer_size);

    if (!device->hidden->foreground_active) {
        return true;
    }

    for (Uint32 channel = 0; channel < device->hidden->playback_channels; ++channel) {
        Sint16 *dst = device->hidden->playback_channel_buffers[channel] + frame_offset;
        for (Uint32 i = 0; i < frame_count; ++i) {
            dst[i] = src[(i * device->hidden->playback_channels) + channel];
        }
        DCFlushRange(dst, slot_bytes);
    }

    device->hidden->write_frame_offset += frame_count;
    if (device->hidden->write_frame_offset >= device->hidden->ring_frames) {
        device->hidden->write_frame_offset = 0;
    }

    return true;
}

static bool WIIUAUDIO_WaitRecordingDevice(SDL_AudioDevice *device)
{
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        MICStatus status;

        if (!SDL_WIIU_IsInForeground()) {
            SDL_Delay(16);
            continue;
        }

        if (MICGetStatus(device->hidden->mic_handle, &status) != MIC_ERROR_OK) {
            return SDL_SetError("MICGetStatus failed while waiting for samples");
        }

        if (status.availableData > 0) {
            return true;
        }

        SDL_Delay(1);
    }

    return true;
}

static int WIIUAUDIO_RecordDevice(SDL_AudioDevice *device, void *buffer, int buflen)
{
    MICStatus status;
    Sint16 *dst = (Sint16 *)buffer;
    const int requested_samples = buflen / (int)sizeof(Sint16);
    int available_samples;
    int copy_samples;
    int first_chunk;
    const MICError mic_error = MICGetStatus(device->hidden->mic_handle, &status);

    if (mic_error != MIC_ERROR_OK) {
        SDL_SetError("MICGetStatus failed: %s", WIIUAUDIO_MICErrorName(mic_error));
        return -1;
    }

    available_samples = status.availableData;
    if (available_samples <= 0) {
        return 0;
    }

    copy_samples = SDL_min(requested_samples, available_samples);
    DCInvalidateRange(device->hidden->mic_sample_buffer,
                      device->hidden->mic_sample_count * sizeof(Sint16));

    first_chunk = SDL_min(copy_samples, (int)device->hidden->mic_sample_count - status.bufferPos);
    SDL_memcpy(dst,
               device->hidden->mic_sample_buffer + status.bufferPos,
               first_chunk * sizeof(Sint16));

    if (first_chunk < copy_samples) {
        SDL_memcpy(dst + first_chunk,
                   device->hidden->mic_sample_buffer,
                   (copy_samples - first_chunk) * sizeof(Sint16));
    }

    if (MICSetDataConsumed(device->hidden->mic_handle, copy_samples) != MIC_ERROR_OK) {
        SDL_SetError("MICSetDataConsumed failed");
        return -1;
    }

    return copy_samples * (int)sizeof(Sint16);
}

static void WIIUAUDIO_FlushRecording(SDL_AudioDevice *device)
{
    MICStatus status;

    if (MICGetStatus(device->hidden->mic_handle, &status) == MIC_ERROR_OK && status.availableData > 0) {
        MICSetDataConsumed(device->hidden->mic_handle, status.availableData);
    }
}

static void WIIUAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (!device->hidden) {
        return;
    }

    if (device->recording) {
        if (device->hidden->mic_handle >= 0) {
            MICClose(device->hidden->mic_handle);
            MICUninit(device->hidden->mic_handle);
            device->hidden->mic_handle = -1;
        }
        if (device->hidden->mic_sample_buffer) {
            MEMFreeToDefaultHeap(device->hidden->mic_sample_buffer);
            device->hidden->mic_sample_buffer = NULL;
        }
    } else {
        WIIUAUDIO_ClosePlaybackDevice(device);
    }

    SDL_free(device->hidden);
    device->hidden = NULL;
}

static void WIIUAUDIO_DetectDevices(SDL_AudioDevice **default_playback, SDL_AudioDevice **default_recording)
{
    SDL_AudioSpec playback_spec;
    Uint32 runtime_sample_rate;
    Uint32 runtime_frame_samples;

    SDL_zero(playback_spec);

    if (SDL_WIIU_AX_Acquire(&runtime_sample_rate, &runtime_frame_samples)) {
        const Uint32 tv_channels = WIIUAUDIO_QueryChannelCount(AX_DEVICE_TYPE_TV, 2);
        (void)runtime_frame_samples;
        playback_spec.format = SDL_AUDIO_S16;
        playback_spec.channels = (Uint8)WIIUAUDIO_GetPreferredPlaybackChannels(tv_channels, tv_channels);
        playback_spec.freq = (int)runtime_sample_rate;
        SDL_WIIU_AX_Release();
        *default_playback = SDL_AddAudioDevice(false, WIIUAUDIO_PLAYBACK_NAME, &playback_spec, (void *)((size_t)0x1));
    } else {
        *default_playback = SDL_AddAudioDevice(false, WIIUAUDIO_PLAYBACK_NAME, NULL, (void *)((size_t)0x1));
    }

    *default_recording = SDL_AddAudioDevice(true, WIIUAUDIO_RECORDING_NAME, NULL, (void *)((size_t)0x2));
}

static bool WIIUAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->DetectDevices = WIIUAUDIO_DetectDevices;
    impl->OpenDevice = WIIUAUDIO_OpenDevice;
    impl->WaitDevice = WIIUAUDIO_WaitDevice;
    impl->PlayDevice = WIIUAUDIO_PlayDevice;
    impl->GetDeviceBuf = WIIUAUDIO_GetDeviceBuf;
    impl->WaitRecordingDevice = WIIUAUDIO_WaitRecordingDevice;
    impl->RecordDevice = WIIUAUDIO_RecordDevice;
    impl->FlushRecording = WIIUAUDIO_FlushRecording;
    impl->CloseDevice = WIIUAUDIO_CloseDevice;
    impl->HasRecordingSupport = true;
    impl->OnlyHasDefaultPlaybackDevice = true;
    impl->OnlyHasDefaultRecordingDevice = true;
    return true;
}

AudioBootStrap WIIUAUDIO_bootstrap = {
    WIIUAUDIO_DRIVER_NAME, "Wii U audio driver", WIIUAUDIO_Init, false, false
};

#endif // SDL_AUDIO_DRIVER_WIIU
