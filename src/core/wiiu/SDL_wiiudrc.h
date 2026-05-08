#include "SDL_internal.h"

#ifndef SDL_wiiudrc_h_
#define SDL_wiiudrc_h_

#include <vpad/input.h>

typedef struct SDL_WiiUDRCState
{
    bool connected;
    bool has_status;
    VPADReadError error;
    Uint64 timestamp_ns;
    VPADStatus status;
    VPADTouchData touch;
} SDL_WiiUDRCState;

extern bool SDL_WIIU_InitDRC(void);
extern void SDL_WIIU_QuitDRC(void);
extern void SDL_WIIU_BeginDRCUpdateCycle(void);
extern const SDL_WiiUDRCState *SDL_WIIU_GetDRCState(void);

#endif // SDL_wiiudrc_h_
