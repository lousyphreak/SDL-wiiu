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

#ifndef SDL_wiiuaudio_h_
#define SDL_wiiuaudio_h_

#include "SDL_internal.h"

#include <coreinit/memres.h>
#include <mic/mic.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>

#define WIIU_AUDIO_MIX_BUFFERS 4
#define WIIU_AUDIO_RING_BUFFERS 4
#define WIIU_AUDIO_MAX_PLAYBACK_CHANNELS 6
#define WIIU_AUDIO_MAX_TV_CHANNELS 6
#define WIIU_AUDIO_MAX_DRC_CHANNELS 4

struct SDL_PrivateAudioData
{
    Uint8 *rawbuf;
    Uint8 *mixbufs[WIIU_AUDIO_MIX_BUFFERS];
    int next_buffer;

    bool foreground_active;
    Uint32 sample_rate;
    Uint32 frame_samples;
    Uint64 frame_duration_ns;
    Uint32 playback_channels;
    Uint32 tv_channels;
    Uint32 drc_channels;
    Uint32 ring_frames;
    Uint32 write_frame_offset;

    Sint16 *playback_channel_buffers[WIIU_AUDIO_MAX_PLAYBACK_CHANNELS];
    AXVoice *voices[WIIU_AUDIO_MAX_PLAYBACK_CHANNELS];

    MEMResource mic_resource;
    MICRingBuffer mic_ring;
    MICHandle mic_handle;
    Sint16 *mic_sample_buffer;
    Uint32 mic_sample_count;
};

#endif // SDL_wiiuaudio_h_
