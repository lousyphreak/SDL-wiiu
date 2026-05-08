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

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "SDL_wiiuvideo.h"
#include "SDL_wiiublitshader.h"

#include "../../SDL_properties_c.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"
#include "../SDL_surface_c.h"
#include <coreinit/memdefaultheap.h>
#include <gfd.h>
#include <gx2/clear.h>
#include <gx2/draw.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <gx2r/surface.h>

#define WIIU_SURFACE "SDL.internal.window.surface"
#define WIIU_BLIT_VERTEX_COUNT 6

static SDL_WiiUScanTarget *SDL_WIIU_GetScanTarget(SDL_VideoData *driverdata, SDL_WiiUDisplayKind kind)
{
    return (kind == SDL_WIIU_DISPLAY_DRC) ? &driverdata->drc : &driverdata->tv;
}

static void SDL_WIIU_DestroyFramebufferTexture(SDL_Window *window)
{
    SDL_WindowData *windowdata = window ? window->internal : NULL;

    if (!windowdata || !windowdata->framebuffer_texture_ready) {
        return;
    }

    GX2RDestroySurfaceEx(&windowdata->framebuffer_texture.surface, 0);
    SDL_zero(windowdata->framebuffer_texture);
    windowdata->framebuffer_texture_ready = false;
}

static void SDL_WIIU_DestroyBlitShaderGroup(SDL_WiiUShaderGroup *group)
{
    if (group->fetch_shader_program) {
        MEMFreeToDefaultHeap(group->fetch_shader_program);
    }

    if (group->pixel_shader) {
        if (group->pixel_shader->gx2rBuffer.buffer) {
            GX2RDestroyBufferEx(&group->pixel_shader->gx2rBuffer, 0);
        }
        MEMFreeToDefaultHeap(group->pixel_shader);
    }

    if (group->vertex_shader) {
        if (group->vertex_shader->gx2rBuffer.buffer) {
            GX2RDestroyBufferEx(&group->vertex_shader->gx2rBuffer, 0);
        }
        MEMFreeToDefaultHeap(group->vertex_shader);
    }

    SDL_zero(*group);
}

static GX2PixelShader *SDL_WIIU_LoadGFDPixelShader(uint32_t index, const void *file)
{
    uint32_t header_size;
    uint32_t program_size;
    GX2PixelShader *shader;
    void *program;

    if (index >= GFDGetPixelShaderCount(file)) {
        SDL_SetError("Wii U shader blob is missing the expected pixel shader");
        return NULL;
    }

    header_size = GFDGetPixelShaderHeaderSize(index, file);
    program_size = GFDGetPixelShaderProgramSize(index, file);
    if (!header_size || !program_size) {
        SDL_SetError("Wii U pixel shader blob is invalid");
        return NULL;
    }

    shader = MEMAllocFromDefaultHeapEx(header_size, 64);
    if (!shader) {
        SDL_SetError("Unable to allocate Wii U pixel shader header");
        return NULL;
    }
    SDL_memset(shader, 0, header_size);

    shader->gx2rBuffer.flags = GX2R_RESOURCE_BIND_SHADER_PROGRAM |
                               GX2R_RESOURCE_USAGE_CPU_READ |
                               GX2R_RESOURCE_USAGE_CPU_WRITE |
                               GX2R_RESOURCE_USAGE_GPU_READ;
    shader->gx2rBuffer.elemSize = program_size;
    shader->gx2rBuffer.elemCount = 1;
    if (!GX2RCreateBuffer(&shader->gx2rBuffer)) {
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to allocate Wii U pixel shader program");
        return NULL;
    }

    program = GX2RLockBufferEx(&shader->gx2rBuffer, 0);
    if (!program) {
        GX2RDestroyBufferEx(&shader->gx2rBuffer, 0);
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to lock Wii U pixel shader program");
        return NULL;
    }

    if (!GFDGetPixelShader(shader, program, index, file)) {
        GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE);
        GX2RDestroyBufferEx(&shader->gx2rBuffer, 0);
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to decode Wii U pixel shader");
        return NULL;
    }

    GX2RUnlockBufferEx(&shader->gx2rBuffer, 0);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->program, shader->size);
    return shader;
}

static GX2VertexShader *SDL_WIIU_LoadGFDVertexShader(uint32_t index, const void *file)
{
    uint32_t header_size;
    uint32_t program_size;
    GX2VertexShader *shader;
    void *program;

    if (index >= GFDGetVertexShaderCount(file)) {
        SDL_SetError("Wii U shader blob is missing the expected vertex shader");
        return NULL;
    }

    header_size = GFDGetVertexShaderHeaderSize(index, file);
    program_size = GFDGetVertexShaderProgramSize(index, file);
    if (!header_size || !program_size) {
        SDL_SetError("Wii U vertex shader blob is invalid");
        return NULL;
    }

    shader = MEMAllocFromDefaultHeapEx(header_size, 64);
    if (!shader) {
        SDL_SetError("Unable to allocate Wii U vertex shader header");
        return NULL;
    }
    SDL_memset(shader, 0, header_size);

    shader->gx2rBuffer.flags = GX2R_RESOURCE_BIND_SHADER_PROGRAM |
                               GX2R_RESOURCE_USAGE_CPU_READ |
                               GX2R_RESOURCE_USAGE_CPU_WRITE |
                               GX2R_RESOURCE_USAGE_GPU_READ;
    shader->gx2rBuffer.elemSize = program_size;
    shader->gx2rBuffer.elemCount = 1;
    if (!GX2RCreateBuffer(&shader->gx2rBuffer)) {
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to allocate Wii U vertex shader program");
        return NULL;
    }

    program = GX2RLockBufferEx(&shader->gx2rBuffer, 0);
    if (!program) {
        GX2RDestroyBufferEx(&shader->gx2rBuffer, 0);
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to lock Wii U vertex shader program");
        return NULL;
    }

    if (!GFDGetVertexShader(shader, program, index, file)) {
        GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE);
        GX2RDestroyBufferEx(&shader->gx2rBuffer, 0);
        MEMFreeToDefaultHeap(shader);
        SDL_SetError("Unable to decode Wii U vertex shader");
        return NULL;
    }

    GX2RUnlockBufferEx(&shader->gx2rBuffer, 0);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->program, shader->size);
    return shader;
}

static bool SDL_WIIU_LoadBlitShaderGroup(SDL_WiiUShaderGroup *group, const void *file)
{
    SDL_zero(*group);

    group->vertex_shader = SDL_WIIU_LoadGFDVertexShader(0, file);
    group->pixel_shader = SDL_WIIU_LoadGFDPixelShader(0, file);
    if (!group->vertex_shader || !group->pixel_shader) {
        SDL_WIIU_DestroyBlitShaderGroup(group);
        return false;
    }
    return true;
}

static uint32_t SDL_WIIU_GetAttribMask(GX2AttribFormat format)
{
    switch (format) {
    case GX2_ATTRIB_FORMAT_UNORM_8:
    case GX2_ATTRIB_FORMAT_UINT_8:
    case GX2_ATTRIB_FORMAT_SNORM_8:
    case GX2_ATTRIB_FORMAT_SINT_8:
    case GX2_ATTRIB_FORMAT_FLOAT_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_UNORM_8_8:
    case GX2_ATTRIB_FORMAT_UINT_8_8:
    case GX2_ATTRIB_FORMAT_SNORM_8_8:
    case GX2_ATTRIB_FORMAT_SINT_8_8:
    case GX2_ATTRIB_FORMAT_FLOAT_32_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1);
    case GX2_ATTRIB_FORMAT_UNORM_8_8_8_8:
    case GX2_ATTRIB_FORMAT_UINT_8_8_8_8:
    case GX2_ATTRIB_FORMAT_SNORM_8_8_8_8:
    case GX2_ATTRIB_FORMAT_SINT_8_8_8_8:
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32:
        return GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    default:
        return GX2_SEL_MASK(GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    }
}

static int32_t SDL_WIIU_GetVertexAttribLocation(const GX2VertexShader *shader, const char *name)
{
    uint32_t i;

    for (i = 0; i < shader->attribVarCount; ++i) {
        if (SDL_strcmp(shader->attribVars[i].name, name) == 0) {
            return shader->attribVars[i].location;
        }
    }
    return -1;
}

static int32_t SDL_WIIU_GetPixelSamplerLocation(const GX2PixelShader *shader, const char *name)
{
    uint32_t i;

    for (i = 0; i < shader->samplerVarCount; ++i) {
        if (SDL_strcmp(shader->samplerVars[i].name, name) == 0) {
            return (int32_t)shader->samplerVars[i].location;
        }
    }
    return -1;
}

static bool SDL_WIIU_InitShaderAttribute(SDL_WiiUShaderGroup *group, const char *name, uint32_t buffer, uint32_t offset, GX2AttribFormat format)
{
    GX2AttribStream *attrib;
    const int32_t location = SDL_WIIU_GetVertexAttribLocation(group->vertex_shader, name);

    if (location < 0) {
        return SDL_SetError("Unable to resolve Wii U shader attribute %s", name);
    }

    attrib = &group->attributes[group->num_attributes++];
    attrib->location = location;
    attrib->buffer = buffer;
    attrib->offset = offset;
    attrib->format = format;
    attrib->type = GX2_ATTRIB_INDEX_PER_VERTEX;
    attrib->aluDivisor = 0;
    attrib->mask = SDL_WIIU_GetAttribMask(format);
    attrib->endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    return true;
}

static bool SDL_WIIU_InitFetchShader(SDL_WiiUShaderGroup *group)
{
    const uint32_t size = GX2CalcFetchShaderSizeEx(group->num_attributes, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);

    group->fetch_shader_program = MEMAllocFromDefaultHeapEx(size, GX2_SHADER_PROGRAM_ALIGNMENT);
    if (!group->fetch_shader_program) {
        return SDL_SetError("Unable to allocate Wii U fetch shader");
    }

    GX2InitFetchShaderEx(&group->fetch_shader,
                         group->fetch_shader_program,
                         group->num_attributes,
                         group->attributes,
                         GX2_FETCH_SHADER_TESSELLATION_NONE,
                         GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, group->fetch_shader_program, size);
    return true;
}

static GX2ShaderMode SDL_WIIU_GetShaderMode(const SDL_WiiUShaderGroup *group)
{
    if ((group->vertex_shader && group->vertex_shader->mode == GX2_SHADER_MODE_UNIFORM_BLOCK) ||
        (group->pixel_shader && group->pixel_shader->mode == GX2_SHADER_MODE_UNIFORM_BLOCK)) {
        return GX2_SHADER_MODE_UNIFORM_BLOCK;
    }

    return GX2_SHADER_MODE_UNIFORM_REGISTER;
}

static void SDL_WIIU_DestroyBlitPipeline(SDL_VideoData *driverdata)
{
    if (driverdata->blit_vertex_buffer.buffer) {
        GX2RDestroyBufferEx(&driverdata->blit_vertex_buffer, 0);
    }

    SDL_WIIU_DestroyBlitShaderGroup(&driverdata->blit_shader);
    driverdata->blit_pipeline_ready = false;
    SDL_zero(driverdata->blit_vertex_buffer);
    SDL_zero(driverdata->blit_sampler);
}

static bool SDL_WIIU_CreateFramebufferTexture(SDL_Window *window, int width, int height)
{
    SDL_WindowData *windowdata = window->internal;
    GX2Texture *texture = &windowdata->framebuffer_texture;

    SDL_WIIU_DestroyFramebufferTexture(window);

    SDL_zero(*texture);
    texture->surface.use = GX2_SURFACE_USE_TEXTURE;
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width = (uint32_t)width;
    texture->surface.height = (uint32_t)height;
    texture->surface.depth = 1;
    texture->surface.mipLevels = 1;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.aa = GX2_AA_MODE1X;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumMips = 1;
    texture->viewNumSlices = 1;
    texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    if (!GX2RCreateSurface(&texture->surface,
                           GX2R_RESOURCE_BIND_TEXTURE |
                               GX2R_RESOURCE_USAGE_CPU_WRITE |
                               GX2R_RESOURCE_USAGE_GPU_READ |
                               GX2R_RESOURCE_USAGE_FORCE_MEM2)) {
        return SDL_SetError("Unable to allocate Wii U framebuffer texture");
    }

    SDL_memset(texture->surface.image, 0, texture->surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    GX2InitTextureRegs(texture);
    windowdata->framebuffer_texture_ready = true;
    return true;
}

bool SDL_WIIU_InitFramebufferRenderer(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    SDL_WiiUBlitVertex *vertices;
    int32_t texture_unit;

    if (driverdata->blit_pipeline_ready) {
        return true;
    }

    if (!SDL_WIIU_LoadBlitShaderGroup(&driverdata->blit_shader, SDL_WIIU_blit_shader_gsh) ||
        !SDL_WIIU_InitShaderAttribute(&driverdata->blit_shader, "a_position", 0, offsetof(SDL_WiiUBlitVertex, position), GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !SDL_WIIU_InitShaderAttribute(&driverdata->blit_shader, "a_texcoord", 0, offsetof(SDL_WiiUBlitVertex, texcoord), GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !SDL_WIIU_InitFetchShader(&driverdata->blit_shader)) {
        SDL_WIIU_DestroyBlitPipeline(driverdata);
        return false;
    }

    texture_unit = SDL_WIIU_GetPixelSamplerLocation(driverdata->blit_shader.pixel_shader, "u_texture");
    if (texture_unit < 0) {
        SDL_WIIU_DestroyBlitPipeline(driverdata);
        return SDL_SetError("Unable to resolve Wii U blit texture sampler");
    }
    driverdata->blit_texture_unit = (uint32_t)texture_unit;

    driverdata->blit_vertex_buffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                           GX2R_RESOURCE_USAGE_CPU_WRITE |
                                           GX2R_RESOURCE_USAGE_GPU_READ |
                                           GX2R_RESOURCE_USAGE_FORCE_MEM2;
    driverdata->blit_vertex_buffer.elemSize = sizeof(SDL_WiiUBlitVertex);
    driverdata->blit_vertex_buffer.elemCount = WIIU_BLIT_VERTEX_COUNT;
    if (!GX2RCreateBuffer(&driverdata->blit_vertex_buffer)) {
        SDL_WIIU_DestroyBlitPipeline(driverdata);
        return SDL_SetError("Unable to allocate Wii U blit vertex buffer");
    }

    vertices = GX2RLockBufferEx(&driverdata->blit_vertex_buffer, 0);
    if (!vertices) {
        SDL_WIIU_DestroyBlitPipeline(driverdata);
        return SDL_SetError("Unable to lock Wii U blit vertex buffer");
    }
    SDL_memset(vertices, 0, sizeof(SDL_WiiUBlitVertex) * WIIU_BLIT_VERTEX_COUNT);
    GX2RUnlockBufferEx(&driverdata->blit_vertex_buffer, 0);

    GX2InitSampler(&driverdata->blit_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    driverdata->blit_pipeline_ready = true;
    return true;
}

void SDL_WIIU_QuitFramebufferRenderer(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;

    for (SDL_Window *window = _this->windows; window; window = window->next) {
        SDL_WIIU_DestroyWindowFramebuffer(_this, window);
    }

    SDL_WIIU_DestroyBlitPipeline(driverdata);
}

static void SDL_WIIU_UpdateBlitVertices(SDL_VideoData *driverdata, const SDL_Rect *dstrect, uint32_t target_width, uint32_t target_height)
{
    SDL_WiiUBlitVertex *vertices = (SDL_WiiUBlitVertex *)GX2RLockBufferEx(&driverdata->blit_vertex_buffer, 0);
    const float x0 = ((2.0f * (float)dstrect->x) / (float)target_width) - 1.0f;
    const float x1 = ((2.0f * (float)(dstrect->x + dstrect->w)) / (float)target_width) - 1.0f;
    const float y0 = 1.0f - ((2.0f * (float)dstrect->y) / (float)target_height);
    const float y1 = 1.0f - ((2.0f * (float)(dstrect->y + dstrect->h)) / (float)target_height);
    static const SDL_WiiUBlitVertex template_vertices[WIIU_BLIT_VERTEX_COUNT] = {
        { { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { { 0.0f, 0.0f }, { 1.0f, 1.0f } },
        { { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { 0.0f, 0.0f }, { 1.0f, 1.0f } },
        { { 0.0f, 0.0f }, { 0.0f, 1.0f } },
    };

    SDL_memcpy(vertices, template_vertices, sizeof(template_vertices));
    vertices[0].position[0] = x0; vertices[0].position[1] = y0;
    vertices[1].position[0] = x1; vertices[1].position[1] = y0;
    vertices[2].position[0] = x1; vertices[2].position[1] = y1;
    vertices[3].position[0] = x0; vertices[3].position[1] = y0;
    vertices[4].position[0] = x1; vertices[4].position[1] = y1;
    vertices[5].position[0] = x0; vertices[5].position[1] = y1;
    GX2RUnlockBufferEx(&driverdata->blit_vertex_buffer, 0);
}

static void SDL_WIIU_BeginRender(void)
{
    uint32_t swap_count;
    uint32_t flip_count;
    OSTime last_flip;
    OSTime last_vsync;

    while (1) {
        GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
        if (flip_count >= swap_count) {
            return;
        }
        GX2WaitForVsync();
    }
}

static bool SDL_WIIU_UploadFramebufferTexture(SDL_Surface *surface, GX2Texture *texture)
{
    const int bytes_per_pixel = SDL_BYTESPERPIXEL(surface->format);
    const int src_pitch = surface->pitch;
    const int dst_pitch = (int)texture->surface.pitch * bytes_per_pixel;
    Uint8 *dst = (Uint8 *)GX2RLockSurfaceEx(&texture->surface, 0, 0);
    const Uint8 *src = (const Uint8 *)surface->pixels;
    int y;

    if (!dst) {
        return SDL_SetError("Unable to lock Wii U framebuffer texture");
    }

    for (y = 0; y < surface->h; ++y) {
        SDL_memcpy(dst + (y * dst_pitch), src + (y * src_pitch), (size_t)surface->w * bytes_per_pixel);
    }

    GX2RUnlockSurfaceEx(&texture->surface, 0, 0);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    return true;
}

static void SDL_WIIU_RenderWindowToTarget(SDL_VideoData *driverdata, SDL_Window *window, SDL_Surface *surface, SDL_WiiUScanTarget *target, GX2ScanTarget scan_target)
{
    SDL_WindowData *windowdata = window->internal;
    SDL_Rect dstrect;

    SDL_WIIU_GetPresentationRect(surface->w, surface->h, (int)target->width, (int)target->height, &dstrect);
    SDL_WIIU_UpdateBlitVertices(driverdata, &dstrect, target->width, target->height);

    GX2SetContextState(target->context_state);
    GX2ClearColor(&target->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
    GX2SetContextState(target->context_state);
    GX2SetColorBuffer(&target->color_buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, target->width, target->height);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,
                       GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD,
                       FALSE,
                       GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetAlphaToMask(FALSE, GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
    GX2SetShaderMode(SDL_WIIU_GetShaderMode(&driverdata->blit_shader));
    GX2SetFetchShader(&driverdata->blit_shader.fetch_shader);
    GX2SetVertexShader(driverdata->blit_shader.vertex_shader);
    GX2SetPixelShader(driverdata->blit_shader.pixel_shader);
    GX2SetPixelTexture(&windowdata->framebuffer_texture, driverdata->blit_texture_unit);
    GX2SetPixelSampler(&driverdata->blit_sampler, driverdata->blit_texture_unit);
    GX2RSetAttributeBuffer(&driverdata->blit_vertex_buffer, 0, sizeof(SDL_WiiUBlitVertex), 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, WIIU_BLIT_VERTEX_COUNT, 0, 1);
    GX2CopyColorBufferToScanBuffer(&target->color_buffer, scan_target);
}

bool SDL_WIIU_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_Surface *framebuffer;
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(SDL_GetDisplayForWindow(window));

    SDL_WIIU_DestroyWindowFramebuffer(_this, window);

    framebuffer = SDL_CreateSurface(display->current_mode->w, display->current_mode->h, display->current_mode->format);
    if (!framebuffer) {
        return false;
    }

    if (!SDL_WIIU_CreateFramebufferTexture(window, framebuffer->w, framebuffer->h)) {
        SDL_DestroySurface(framebuffer);
        return false;
    }

    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), WIIU_SURFACE, framebuffer);
    *format = framebuffer->format;
    *pixels = framebuffer->pixels;
    *pitch = framebuffer->pitch;
    return true;
}

bool SDL_WIIU_SetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int vsync)
{
    SDL_WindowData *windowdata = window->internal;
    (void)_this;

    if (vsync < 0) {
        return SDL_Unsupported();
    }

    windowdata->framebuffer_vsync = vsync ? 1 : 0;
    GX2SetSwapInterval(windowdata->framebuffer_vsync ? 1 : 0);
    return true;
}

bool SDL_WIIU_GetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int *vsync)
{
    SDL_WindowData *windowdata = window->internal;
    (void)_this;

    if (vsync) {
        *vsync = windowdata->framebuffer_vsync;
    }
    return true;
}

bool SDL_WIIU_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    SDL_Surface *surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), WIIU_SURFACE, NULL);
    SDL_WindowData *windowdata = window->internal;
    SDL_WiiUScanTarget *tv_target;
    SDL_WiiUScanTarget *drc_target;
    const bool render_tv = SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV) == window;
    const bool render_drc = driverdata->drc_connected && (SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC) == window);

    (void)rects;
    (void)numrects;

    if (!surface) {
        return SDL_SetError("Wii U window framebuffer surface is unavailable");
    }

    if (!driverdata->foreground_ready || !SDL_WIIU_IsInForeground()) {
        return true;
    }

    if (!windowdata || !windowdata->framebuffer_texture_ready) {
        return SDL_SetError("Wii U framebuffer texture is unavailable");
    }

    tv_target = SDL_WIIU_GetScanTarget(driverdata, SDL_WIIU_DISPLAY_TV);
    drc_target = SDL_WIIU_GetScanTarget(driverdata, SDL_WIIU_DISPLAY_DRC);
    if ((!render_tv || !tv_target->scan_buffer) && (!render_drc || !drc_target->scan_buffer)) {
        return true;
    }

    if (!SDL_WIIU_InitFramebufferRenderer(_this) ||
        !SDL_WIIU_UploadFramebufferTexture(surface, &windowdata->framebuffer_texture)) {
        return false;
    }

    SDL_WIIU_BeginRender();
    if (render_tv && tv_target->scan_buffer) {
        SDL_WIIU_RenderWindowToTarget(driverdata, window, surface, tv_target, GX2_SCAN_TARGET_TV);
    }
    if (render_drc && drc_target->scan_buffer) {
        SDL_WIIU_RenderWindowToTarget(driverdata, window, surface, drc_target, GX2_SCAN_TARGET_DRC);
    }

    GX2SwapScanBuffers();
    GX2Flush();
    GX2DrawDone();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(driverdata->drc_connected ? TRUE : FALSE);
    return true;
}

void SDL_WIIU_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_Surface *surface;

    (void)_this;

    surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), WIIU_SURFACE, NULL);
    if (surface) {
        SDL_DestroySurface(surface);
    }

    SDL_WIIU_DestroyFramebufferTexture(window);
    SDL_ClearProperty(SDL_GetWindowProperties(window), WIIU_SURFACE);
}

#endif // SDL_VIDEO_DRIVER_WIIU
