#ifndef SDL_wiiugfx_h_
#define SDL_wiiugfx_h_

#include "SDL_internal.h"

#include "SDL_wiiuvideo.h"

#ifdef SDL_VIDEO_DRIVER_WIIU

#include <gx2/shaders.h>

extern void SDL_WIIU_DestroyShaderGroup(SDL_WiiUShaderGroup *group);
extern bool SDL_WIIU_LoadShaderGroup(SDL_WiiUShaderGroup *group, const void *file);
extern int32_t SDL_WIIU_GetPixelSamplerLocation(const GX2PixelShader *shader, const char *name);
extern bool SDL_WIIU_InitShaderAttribute(SDL_WiiUShaderGroup *group, const char *name, uint32_t buffer, uint32_t offset, GX2AttribFormat format);
extern bool SDL_WIIU_InitFetchShader(SDL_WiiUShaderGroup *group);
extern GX2ShaderMode SDL_WIIU_GetShaderMode(const SDL_WiiUShaderGroup *group);
extern void SDL_WIIU_BeginRender(void);

#endif // SDL_VIDEO_DRIVER_WIIU

#endif // SDL_wiiugfx_h_
