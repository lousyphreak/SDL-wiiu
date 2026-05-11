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

#ifndef SDL_wiiuvideo_h_
#define SDL_wiiuvideo_h_

#include "../SDL_sysvideo.h"

#include <coreinit/memheap.h>
#include <gx2/context.h>
#include <gx2/enum.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>

#define WIIU_SCAN_BUFFER_COUNT 2
#define SDL_WIIU_PRESENT_OUTPUT_TV 0x1u
#define SDL_WIIU_PRESENT_OUTPUT_DRC 0x2u

typedef enum SDL_WiiUDisplayKind
{
    SDL_WIIU_DISPLAY_TV,
    SDL_WIIU_DISPLAY_DRC
} SDL_WiiUDisplayKind;

typedef enum SDL_WiiUWindowRole
{
    SDL_WIIU_WINDOW_ROLE_SHARED,
    SDL_WIIU_WINDOW_ROLE_DISPLAY
} SDL_WiiUWindowRole;

typedef struct SDL_WiiUShaderGroup
{
    GX2FetchShader fetch_shader;
    void *fetch_shader_program;
    GX2PixelShader *pixel_shader;
    GX2VertexShader *vertex_shader;
    uint32_t num_attributes;
    GX2AttribStream attributes[16];
} SDL_WiiUShaderGroup;

typedef struct SDL_WiiUBlitVertex
{
    float position[2];
    float texcoord[2];
} SDL_WiiUBlitVertex;

typedef struct SDL_WiiUScanTarget
{
    SDL_WiiUDisplayKind kind;
    uint32_t width;
    uint32_t height;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t scan_width;
    uint32_t scan_height;
    GX2SurfaceFormat surface_format;
    GX2TVRenderMode tv_render_mode;
    GX2DrcRenderMode drc_render_mode;
    void *scan_buffer;
    uint32_t total_size;
    GX2ColorBuffer color_buffer;
    GX2ContextState *context_state;
} SDL_WiiUScanTarget;

struct SDL_VideoData
{
    SDL_DisplayID tv_display;
    SDL_DisplayID drc_display;
    void *command_buffer_pool;
    MEMHeapHandle foreground_heap;
    MEMHeapHandle mem1_heap;
    bool gx2_initialized;
    bool foreground_ready;
    bool vpad_initialized;
    bool keyboard_initialized;
    bool mouse_initialized;
    bool drc_connected;
    bool drc_uses_tv_render_buffer;
    Uint32 pending_present_mask;
    Uint32 foreground_generation;
    SDL_WiiUScanTarget tv;
    SDL_WiiUScanTarget drc;
    SDL_WiiUShaderGroup blit_shader;
    GX2Sampler blit_sampler;
    GX2RBuffer blit_vertex_buffer;
    uint32_t blit_texture_unit;
    bool blit_pipeline_ready;
};

struct SDL_DisplayData
{
    SDL_WiiUDisplayKind kind;
    int x;
    int y;
};

struct SDL_WindowData
{
    SDL_DisplayID display_id;
    SDL_WiiUDisplayKind display_kind;
    SDL_WiiUWindowRole role;
    int framebuffer_vsync;
    GX2Texture framebuffer_texture;
    bool framebuffer_texture_ready;
};

extern bool SDL_WIIU_InitFramebufferRenderer(SDL_VideoDevice *_this);
extern void SDL_WIIU_QuitFramebufferRenderer(SDL_VideoDevice *_this);
extern bool SDL_WIIU_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch);
extern bool SDL_WIIU_SetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int vsync);
extern bool SDL_WIIU_GetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int *vsync);
extern bool SDL_WIIU_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects);
extern void SDL_WIIU_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window);

extern void SDL_WIIU_PumpEvents(SDL_VideoDevice *_this);

extern SDL_Window *SDL_WIIU_GetOutputWindow(SDL_VideoDevice *_this, SDL_WiiUDisplayKind kind);
extern bool SDL_WIIU_UpdateOutputWindows(SDL_VideoDevice *_this);
extern void SDL_WIIU_GetPresentationRect(int src_w, int src_h, int dst_w, int dst_h, SDL_Rect *rect);
extern void SDL_WIIU_GetWindowPresentationSize(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h);
extern Uint32 SDL_WIIU_GetPresentOutputMask(SDL_VideoDevice *_this);
extern bool SDL_WIIU_ShouldSwapPresent(SDL_VideoDevice *_this, Uint32 rendered_outputs);
extern void SDL_WIIU_ResetPresentState(SDL_VideoDevice *_this);

extern void SDL_WIIU_InitTouch(void);
extern void SDL_WIIU_QuitTouch(void);
extern void SDL_WIIU_PollTouch(SDL_VideoDevice *_this);
extern void SDL_WIIU_SuspendTouch(void);

extern bool SDL_WIIU_InitMouse(SDL_VideoDevice *_this);
extern void SDL_WIIU_QuitMouse(void);
extern void SDL_WIIU_PollMouse(SDL_VideoDevice *_this);
extern void SDL_WIIU_SuspendMouse(void);

extern bool SDL_WIIU_InitKeyboard(SDL_VideoDevice *_this);
extern void SDL_WIIU_QuitKeyboard(void);

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDL_WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this);
extern bool SDL_WIIU_ShowTextInput(SDL_VideoDevice *_this, SDL_PropertiesID props);
extern bool SDL_WIIU_ShowMessageBox(SDL_VideoDevice *_this, const SDL_MessageBoxData *messageboxdata, int *buttonID);
extern void SDL_WIIU_QuitTextUI(void);

#ifdef __cplusplus
}
#endif

#endif // SDL_wiiuvideo_h_
