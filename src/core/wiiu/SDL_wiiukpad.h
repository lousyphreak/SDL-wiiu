#include "SDL_internal.h"

#ifndef SDL_wiiukpad_h_
#define SDL_wiiukpad_h_

#include <padscore/kpad.h>

#define WIIU_MAX_KPAD_CHANNELS 7

typedef struct SDL_WiiUKPADState
{
    bool connected;
    bool has_status;
    KPADError read_error;
    Uint32 sample_count;
    Uint64 timestamp_ns;
    KPADStatus status;
} SDL_WiiUKPADState;

extern bool SDL_WIIU_InitKPAD(void);
extern void SDL_WIIU_QuitKPAD(void);
extern void SDL_WIIU_KPADForegroundReleased(void);
extern bool SDL_WIIU_KPADForegroundAcquired(void);
extern void SDL_WIIU_BeginKPADUpdateCycle(void);
extern const SDL_WiiUKPADState *SDL_WIIU_GetKPADState(int channel);
extern bool SDL_WIIU_EnsureKPADDataFormat(int channel, WPADDataFormat format, bool enable_dpd);

#endif // SDL_wiiukpad_h_
