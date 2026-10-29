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

/**
 * # CategoryWiiU
 *
 * Functions for using GX2 directly with SDL-managed Wii U windows.
 *
 * Use SDL_GetWindowProperties() with the `SDL_PROP_WINDOW_WIIU_*` properties
 * from SDL_video.h to retrieve the current GX2 color buffers and context
 * states owned by SDL for a window. With external graphics enabled, the color
 * descriptors describe the required layout but have no image allocation; the
 * external renderer supplies its color buffer at presentation. These handles
 * can change whenever SDL
 * rebuilds foreground-owned GX2 resources or reroutes TV/DRC output, so
 * applications should refresh cached handles when
 * `SDL_PROP_WINDOW_WIIU_FOREGROUND_GENERATION_NUMBER` changes.
 */

#ifndef SDL_wiiu_h_
#define SDL_wiiu_h_

#include <SDL3/SDL_video.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optional external GX2 render storage, passed through
 * SDL_PROP_WINDOW_CREATE_WIIU_EXTERNAL_GRAPHICS_POINTER. Do not combine this
 * mode with SDL_Renderer or software window surfaces. Keep this structure
 * alive until the window is destroyed. SDL still owns scan buffers and swapping.
 *
 * Set color before SDL_WIIU_SwapWindow(); native scan copying can resolve MSAA.
 * SDL calls release on the main thread after draining GX2 and before destroying
 * its MEM1 heap, including output-mode rebuilds and window destruction. The
 * SDL_PROP_WINDOW_WIIU_MEM1_HEAP_POINTER property exposes that expansion heap.
 * SDL places external graphics scan buffers in foreground memory to reserve
 * MEM1 for render/depth/MSAA storage. Release
 * all external allocations from that heap without issuing GX2 commands. Recreate
 * them from current window properties after foreground acquisition. NULL color
 * suppresses presentation while the external renderer has no target.
 */
struct GX2ColorBuffer;
typedef struct SDL_WiiUExternalGraphics
{
    const struct GX2ColorBuffer *color;
    void (SDLCALL *release)(void *userdata);
    void *userdata;
} SDL_WiiUExternalGraphics;

/** Native keyboard operations dispatched to an external GX2 owner. */
typedef enum SDL_WiiUTextInputGraphicsOperation
{
    SDL_WIIU_TEXT_INPUT_GRAPHICS_INITIALIZE,
    SDL_WIIU_TEXT_INPUT_GRAPHICS_UPDATE,
    SDL_WIIU_TEXT_INPUT_GRAPHICS_SHUTDOWN
} SDL_WiiUTextInputGraphicsOperation;

typedef void (SDLCALL *SDL_WiiUTextInputCallback)(void *userdata);
typedef void (SDLCALL *SDL_WiiUTextInputDrawCallback)(void);

/**
 * Native keyboard graphics bridge for SDL_HINT_VIDEO_EXTERNAL_GRAPHICS.
 *
 * Set a persistent pointer on SDL_GetWindowProperties(window) using
 * SDL_PROP_WINDOW_WIIU_TEXT_INPUT_GRAPHICS_POINTER before starting text input.
 * SDL owns keyboard configuration, controller input and text delivery. The
 * external renderer retains its GX2 session, heaps and presentation buffers.
 *
 * run synchronously invokes callback exactly once on the GX2 submission thread,
 * outside display lists and serialized with rendering, then restores renderer
 * state. It returns false without invoking callback if graphics are unavailable.
 * INITIALIZE precedes UPDATE operations; SHUTDOWN follows every invoked
 * INITIALIZE, including initialization failure or foreground loss. SHUTDOWN must
 * still execute its cleanup callback when foreground storage has been released;
 * it must not submit graphics commands in that case.
 *
 * draw is called from inside an UPDATE callback on the submission thread. It
 * binds a single-sample native UI target for each connected output, calls the
 * corresponding draw callback with GX2 context shadowing disabled, restores
 * context state, and presents through its existing scan buffers. TV and DRC
 * callbacks differ; do not mirror the TV keyboard image to the DRC. The native
 * keyboard uses linear colors. Return false on allocation/presentation failure.
 *
 * SDL calls run on the SDL main thread and waits for completion. Neither
 * callback may pump SDL events, reenter text input or call API-thread rendering
 * functions. The bridge must remain alive until SDL_StopTextInput() and any
 * blocking SDL_StartTextInputWithProperties() call have returned. It may free
 * modal render storage on SHUTDOWN. No second GX2 session is needed.
 *
 * See docs/README-wiiu.md for text submission and cancellation semantics.
 */
typedef struct SDL_WiiUTextInputGraphics
{
    bool (SDLCALL *run)(void *userdata, SDL_WiiUTextInputGraphicsOperation operation,
                       SDL_WiiUTextInputCallback callback, void *callback_userdata);
    bool (SDLCALL *draw)(void *userdata, SDL_WiiUTextInputDrawCallback draw_tv,
                        SDL_WiiUTextInputDrawCallback draw_drc);
    void *userdata;
} SDL_WiiUTextInputGraphics;

#define SDL_PROP_WINDOW_WIIU_TEXT_INPUT_GRAPHICS_POINTER "SDL.window.wiiu.text_input_graphics"

typedef Uint32 SDL_WiiUPresentOutputFlags;

#define SDL_WIIU_PRESENT_OUTPUT_NONE 0x0u
#define SDL_WIIU_PRESENT_OUTPUT_TV   0x1u
#define SDL_WIIU_PRESENT_OUTPUT_DRC  0x2u

/**
 * Present the current GX2 color buffers for a Wii U window.
 *
 * This copies the window's active GX2 color buffers to the SDL-managed scan
 * buffers and performs the swap/present sequence for the outputs currently
 * owned by the window.
 *
 * If TV and DRC are being driven by separate SDL windows, each active output
 * window should call this once per frame after finishing its GX2 rendering.
 *
 * \param window the window to present.
 * \returns true on success or false on failure; call SDL_GetError() for more
 *          information.
 *
 * \threadsafety This function should only be called on the main thread.
 *
 * \since This function is available since SDL 3.5.0.
 *
 * \sa SDL_GetWindowProperties
 */
extern SDL_DECLSPEC bool SDLCALL SDL_WIIU_SwapWindow(SDL_Window *window);

/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_wiiu_h_ */
