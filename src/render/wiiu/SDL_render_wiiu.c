#include "SDL_internal.h"

#ifdef SDL_VIDEO_RENDER_WIIU

#include "../SDL_sysrender.h"

#include "../../video/SDL_pixels_c.h"
#include "../../video/SDL_yuv_c.h"
#include "../../video/SDL_sysvideo.h"
#include "../../video/wiiu/SDL_wiiugfx.h"
#include "../../video/wiiu/SDL_wiiuvideo.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"

#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/display.h>
#include <gx2/draw.h>
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

#include "SDL_wiiurender_shader_gsh.h"

#define WIIU_RENDER_PRESENT_VERTEX_COUNT 6
#define WIIU_TEXTURE_TYPE_NONE 0.0f
#define WIIU_TEXTURE_TYPE_RGBA 1.0f
#define WIIU_TEXTURE_TYPE_INDEX8 2.0f
#define WIIU_TEXTURE_TYPE_YUV3 3.0f
#define WIIU_TEXTURE_TYPE_NV12 4.0f
#define WIIU_TEXTURE_TYPE_P010 5.0f
#define WIIU_UNIFORM_BLOCK_ALIGNMENT 0x100u
#define WIIU_UNIFORM_UPLOAD_BUFFER_SIZE (1024 * 1024)

typedef struct WIIU_Vertex
{
    SDL_FPoint position;
    SDL_FColor color;
    SDL_FPoint tex_coord;
} WIIU_Vertex;

typedef struct WIIU_VertexUniformBlock
{
    float transform[4];
} WIIU_VertexUniformBlock;

typedef struct WIIU_PixelUniformBlock
{
    float texture_type[4];
    float texel_size[4];
    float yuv_offset[4];
    float yuv_r_coeff[4];
    float yuv_g_coeff[4];
    float yuv_b_coeff[4];
} WIIU_PixelUniformBlock;

typedef enum WIIU_TextureKind
{
    WIIU_TEXTURE_KIND_RGBA,
    WIIU_TEXTURE_KIND_INDEX8,
    WIIU_TEXTURE_KIND_YUV3,
    WIIU_TEXTURE_KIND_NV12,
    WIIU_TEXTURE_KIND_P010
} WIIU_TextureKind;

typedef struct WIIU_TextureData
{
    WIIU_TextureKind kind;
    GX2Texture texture;
    GX2Texture texture_u;
    GX2Texture texture_v;
    GX2ColorBuffer color_buffer;
    bool color_buffer_ready;
    Uint32 bytes_per_pixel;
    Uint32 uv_bytes_per_pixel;
    void *shadow_pixels;
    size_t shadow_size;
    int shadow_pitch;
    SDL_SW_YUVTexture *shadow_yuv;
    SDL_Rect locked_rect;
    bool locked;
} WIIU_TextureData;

typedef struct WIIU_PaletteData
{
    GX2Texture texture;
    bool ready;
} WIIU_PaletteData;

typedef struct WIIU_RenderData
{
    SDL_VideoDevice *device;
    SDL_VideoData *video;
    GX2ContextState *context_state;
    SDL_WiiUShaderGroup shader;
    GX2RBuffer vertex_buffer;
    size_t vertex_buffer_size;
    GX2RBuffer present_buffer;
    void *uniform_upload_buffer;
    uint32_t uniform_upload_buffer_size;
    uint32_t uniform_upload_offset;
    uint32_t sampler0_location;
    uint32_t sampler1_location;
    uint32_t sampler2_location;
    GX2UniformBlock *vertex_uniform_block;
    GX2UniformBlock *pixel_uniform_block;
    uint32_t vertex_uniform_block_size;
    uint32_t pixel_uniform_block_size;
    GX2Sampler sampler;
    SDL_ScaleMode sampler_scale_mode;
    SDL_TextureAddressMode sampler_address_u;
    SDL_TextureAddressMode sampler_address_v;
    bool sampler_indexed;
    GX2Sampler palette_sampler;
    WIIU_TextureData backbuffer;
    int backbuffer_w;
    int backbuffer_h;
    SDL_FColor draw_color;
    SDL_Rect viewport;
    SDL_Rect clip_rect;
    bool clip_enabled;
    bool viewport_dirty;
    bool clip_dirty;
    SDL_BlendMode blend_mode;
    bool blend_dirty;
    int vsync;
    Uint32 logged_foreground_generation_present;
} WIIU_RenderData;

static bool WIIU_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props);
static void WIIU_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event);
static bool WIIU_GetOutputSize(SDL_Renderer *renderer, int *w, int *h);
static bool WIIU_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode);
static bool WIIU_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props);
static bool WIIU_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
static bool WIIU_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const Uint8 *Yplane, int Ypitch, const Uint8 *Uplane, int Upitch, const Uint8 *Vplane, int Vpitch);
static bool WIIU_UpdateTextureNV(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const Uint8 *Yplane, int Ypitch, const Uint8 *UVplane, int UVpitch);
static bool WIIU_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch);
static void WIIU_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture);
static bool WIIU_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture);
static bool WIIU_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd);
static bool WIIU_QueueSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd);
static bool WIIU_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count);
static bool WIIU_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count);
static bool WIIU_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count);
static bool WIIU_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect);
static bool WIIU_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect, const double angle, const SDL_FPoint *center, const SDL_FlipMode flip, float scale_x, float scale_y);
static bool WIIU_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride, int num_vertices, const void *indices, int num_indices, int size_indices, float scale_x, float scale_y);
static void WIIU_InvalidateCachedState(SDL_Renderer *renderer);
static bool WIIU_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize);
static bool WIIU_CreatePalette(SDL_Renderer *renderer, SDL_TexturePalette *palette);
static bool WIIU_UpdatePalette(SDL_Renderer *renderer, SDL_TexturePalette *palette, int ncolors, SDL_Color *colors);
static void WIIU_DestroyPalette(SDL_Renderer *renderer, SDL_TexturePalette *palette);
static bool WIIU_ChangeTexturePalette(SDL_Renderer *renderer, SDL_Texture *texture);
static SDL_Surface *WIIU_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect);
static bool WIIU_RenderPresent(SDL_Renderer *renderer);
static void WIIU_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture);
static void WIIU_DestroyRenderer(SDL_Renderer *renderer);
static bool WIIU_SetVSync(SDL_Renderer *renderer, const int vsync);

SDL_RenderDriver WIIU_RenderDriver = {
    WIIU_CreateRenderer, "wiiu"
};

typedef struct WIIU_FormatInfo
{
    GX2SurfaceFormat surface_format;
    Uint32 bytes_per_pixel;
    uint32_t comp_map;
    WIIU_TextureKind kind;
    bool render_target;
} WIIU_FormatInfo;

SDL_COMPILE_TIME_ASSERT(wiiu_vertex_uniform_block_size, sizeof(WIIU_VertexUniformBlock) == (sizeof(float) * 4));
SDL_COMPILE_TIME_ASSERT(wiiu_pixel_uniform_block_size, sizeof(WIIU_PixelUniformBlock) == (sizeof(float) * 24));

static uint32_t WIIU_AlignUniformBlockSize(uint32_t size)
{
    if (size == 0) {
        return 0;
    }

    size += WIIU_UNIFORM_BLOCK_ALIGNMENT - 1u;
    return size & ~(WIIU_UNIFORM_BLOCK_ALIGNMENT - 1u);
}

static bool WIIU_CreateUniformUploadBuffer(WIIU_RenderData *data, uint32_t size)
{
    data->uniform_upload_buffer = MEMAllocFromDefaultHeapEx(size, WIIU_UNIFORM_BLOCK_ALIGNMENT);
    if (!data->uniform_upload_buffer) {
        return SDL_SetError("Unable to allocate Wii U uniform upload buffer");
    }

    SDL_memset(data->uniform_upload_buffer, 0, size);
    data->uniform_upload_buffer_size = size;
    data->uniform_upload_offset = 0;
    return true;
}

static void *WIIU_AllocUniformUpload(WIIU_RenderData *data, uint32_t size)
{
    void *block;

    size = WIIU_AlignUniformBlockSize(size);
    if (size == 0) {
        return NULL;
    }

    if (!data->uniform_upload_buffer || size > data->uniform_upload_buffer_size) {
        SDL_SetError("Wii U uniform upload buffer is too small");
        return NULL;
    }

    if ((data->uniform_upload_offset + size) > data->uniform_upload_buffer_size) {
        data->uniform_upload_offset = 0;
    }

    block = (Uint8 *)data->uniform_upload_buffer + data->uniform_upload_offset;
    SDL_memset(block, 0, size);
    data->uniform_upload_offset += size;
    return block;
}

static void WIIU_StoreUniformFloats(float *dst, const float *src, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; ++i) {
        dst[i] = SDL_SwapFloatLE(src[i]);
    }
}

static bool WIIU_EnsureVertexBuffer(WIIU_RenderData *data, size_t size)
{
    if (size <= data->vertex_buffer_size) {
        return true;
    }

    if (data->vertex_buffer.buffer) {
        GX2RDestroyBufferEx(&data->vertex_buffer, 0);
        SDL_zero(data->vertex_buffer);
        data->vertex_buffer_size = 0;
    }

    data->vertex_buffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                GX2R_RESOURCE_USAGE_CPU_WRITE |
                                GX2R_RESOURCE_USAGE_GPU_READ |
                                GX2R_RESOURCE_USAGE_FORCE_MEM2;
    data->vertex_buffer.elemSize = (uint32_t)size;
    data->vertex_buffer.elemCount = 1;
    if (!GX2RCreateBuffer(&data->vertex_buffer)) {
        return SDL_SetError("Unable to allocate Wii U render vertex buffer");
    }

    data->vertex_buffer_size = size;
    return true;
}

static const WIIU_FormatInfo *WIIU_GetFormatInfo(SDL_PixelFormat format)
{
    static const WIIU_FormatInfo formats[] = {
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A), WIIU_TEXTURE_KIND_RGBA, true },
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* RGBX8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_R), WIIU_TEXTURE_KIND_RGBA, false }, /* ARGB8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* XRGB8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A), WIIU_TEXTURE_KIND_RGBA, false }, /* BGRA8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* BGRX8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_A, GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R), WIIU_TEXTURE_KIND_RGBA, false }, /* ABGR8888 */
        { GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, 4, GX2_COMP_MAP(GX2_SQ_SEL_A, GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* XBGR8888 */
        { GX2_SURFACE_FORMAT_UNORM_R5_G6_B5, 2, GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* RGB565 */
        { GX2_SURFACE_FORMAT_UNORM_R5_G6_B5, 2, GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_1), WIIU_TEXTURE_KIND_RGBA, false }, /* BGR565 */
    };

    switch (format) {
    case SDL_PIXELFORMAT_RGBA8888:
        return &formats[0];
    case SDL_PIXELFORMAT_RGBX8888:
        return &formats[1];
    case SDL_PIXELFORMAT_ARGB8888:
        return &formats[2];
    case SDL_PIXELFORMAT_XRGB8888:
        return &formats[3];
    case SDL_PIXELFORMAT_BGRA8888:
        return &formats[4];
    case SDL_PIXELFORMAT_BGRX8888:
        return &formats[5];
    case SDL_PIXELFORMAT_ABGR8888:
        return &formats[6];
    case SDL_PIXELFORMAT_XBGR8888:
        return &formats[7];
    case SDL_PIXELFORMAT_RGB565:
        return &formats[8];
    case SDL_PIXELFORMAT_BGR565:
        return &formats[9];
    default:
        return NULL;
    }
}

static bool WIIU_GetBlendFactor(SDL_BlendFactor factor, GX2BlendMode *gx2_factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        *gx2_factor = GX2_BLEND_MODE_ZERO;
        return true;
    case SDL_BLENDFACTOR_ONE:
        *gx2_factor = GX2_BLEND_MODE_ONE;
        return true;
    case SDL_BLENDFACTOR_SRC_COLOR:
        *gx2_factor = GX2_BLEND_MODE_SRC_COLOR;
        return true;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        *gx2_factor = GX2_BLEND_MODE_INV_SRC_COLOR;
        return true;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        *gx2_factor = GX2_BLEND_MODE_SRC_ALPHA;
        return true;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        *gx2_factor = GX2_BLEND_MODE_INV_SRC_ALPHA;
        return true;
    case SDL_BLENDFACTOR_DST_COLOR:
        *gx2_factor = GX2_BLEND_MODE_DST_COLOR;
        return true;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        *gx2_factor = GX2_BLEND_MODE_INV_DST_COLOR;
        return true;
    case SDL_BLENDFACTOR_DST_ALPHA:
        *gx2_factor = GX2_BLEND_MODE_DST_ALPHA;
        return true;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        *gx2_factor = GX2_BLEND_MODE_INV_DST_ALPHA;
        return true;
    default:
        return false;
    }
}

static bool WIIU_GetBlendOp(SDL_BlendOperation op, GX2BlendCombineMode *gx2_op)
{
    switch (op) {
    case SDL_BLENDOPERATION_ADD:
        *gx2_op = GX2_BLEND_COMBINE_MODE_ADD;
        return true;
    case SDL_BLENDOPERATION_SUBTRACT:
        *gx2_op = GX2_BLEND_COMBINE_MODE_SUB;
        return true;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        *gx2_op = GX2_BLEND_COMBINE_MODE_REV_SUB;
        return true;
    case SDL_BLENDOPERATION_MINIMUM:
        *gx2_op = GX2_BLEND_COMBINE_MODE_MIN;
        return true;
    case SDL_BLENDOPERATION_MAXIMUM:
        *gx2_op = GX2_BLEND_COMBINE_MODE_MAX;
        return true;
    default:
        return false;
    }
}

static GX2TexClampMode WIIU_GetClampMode(SDL_TextureAddressMode mode)
{
    switch (mode) {
    case SDL_TEXTURE_ADDRESS_WRAP:
        return GX2_TEX_CLAMP_MODE_WRAP;
    case SDL_TEXTURE_ADDRESS_CLAMP:
    default:
        return GX2_TEX_CLAMP_MODE_CLAMP;
    }
}

static bool WIIU_CreateGX2Texture(GX2Texture *texture, int w, int h, GX2SurfaceFormat format, uint32_t comp_map, bool render_target)
{
    SDL_zero(*texture);
    texture->surface.use = render_target ? (GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER) : GX2_SURFACE_USE_TEXTURE;
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width = (uint32_t)w;
    texture->surface.height = (uint32_t)h;
    texture->surface.depth = 1;
    texture->surface.mipLevels = 1;
    texture->surface.format = format;
    texture->surface.aa = GX2_AA_MODE1X;
    texture->surface.tileMode = render_target ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumMips = 1;
    texture->viewNumSlices = 1;
    texture->compMap = comp_map;

    if (!GX2RCreateSurface(&texture->surface,
                           GX2R_RESOURCE_BIND_TEXTURE |
                               (render_target ? GX2R_RESOURCE_BIND_COLOR_BUFFER : GX2R_RESOURCE_BIND_NONE) |
                               GX2R_RESOURCE_USAGE_CPU_WRITE |
                               GX2R_RESOURCE_USAGE_GPU_READ |
                               (render_target ? GX2R_RESOURCE_USAGE_GPU_WRITE : GX2R_RESOURCE_BIND_NONE) |
                               GX2R_RESOURCE_USAGE_FORCE_MEM2)) {
        return SDL_SetError("Unable to allocate Wii U texture");
    }

    SDL_memset(texture->surface.image, 0, texture->surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    GX2InitTextureRegs(texture);
    return true;
}

static void WIIU_DestroyGX2Texture(GX2Texture *texture)
{
    if (texture->surface.image) {
        GX2RDestroySurfaceEx(&texture->surface, 0);
    }
    SDL_zero(*texture);
}

static bool WIIU_InitColorBufferFromTexture(WIIU_TextureData *texturedata)
{
    SDL_zero(texturedata->color_buffer);
    texturedata->color_buffer.surface = texturedata->texture.surface;
    texturedata->color_buffer.viewNumSlices = 1;
    GX2InitColorBufferRegs(&texturedata->color_buffer);
    texturedata->color_buffer_ready = true;
    return true;
}

static void WIIU_DestroyTextureData(WIIU_TextureData *texturedata)
{
    if (!texturedata) {
        return;
    }

    if (texturedata->shadow_yuv) {
        SDL_SW_DestroyYUVTexture(texturedata->shadow_yuv);
    }
    if (texturedata->shadow_pixels) {
        SDL_free(texturedata->shadow_pixels);
    }

    WIIU_DestroyGX2Texture(&texturedata->texture_v);
    WIIU_DestroyGX2Texture(&texturedata->texture_u);
    WIIU_DestroyGX2Texture(&texturedata->texture);
    SDL_zero(*texturedata);
}

static bool WIIU_UpdatePlane(GX2Texture *texture, int x, int y, int w, int h, const void *pixels, int pitch, Uint32 bytes_per_pixel)
{
    Uint8 *dst = (Uint8 *)GX2RLockSurfaceEx(&texture->surface, 0, 0);
    const Uint8 *src = (const Uint8 *)pixels;
    const int dst_pitch = (int)texture->surface.pitch * (int)bytes_per_pixel;
    int row;

    if (!dst) {
        return SDL_SetError("Unable to lock Wii U texture surface");
    }

    dst += y * dst_pitch;
    dst += x * (int)bytes_per_pixel;
    for (row = 0; row < h; ++row) {
        SDL_memcpy(dst, src, (size_t)w * bytes_per_pixel);
        dst += dst_pitch;
        src += pitch;
    }

    GX2RUnlockSurfaceEx(&texture->surface, 0, 0);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    return true;
}

static bool WIIU_EnsureStreamingShadow(SDL_Texture *texture, WIIU_TextureData *texturedata)
{
    if (texturedata->shadow_pixels || texturedata->shadow_yuv) {
        return true;
    }

    if (texturedata->kind == WIIU_TEXTURE_KIND_YUV3 || texturedata->kind == WIIU_TEXTURE_KIND_NV12) {
        texturedata->shadow_yuv = SDL_SW_CreateYUVTexture(texture->format, texture->colorspace, texture->w, texture->h);
        return texturedata->shadow_yuv != NULL;
    }

    if (texturedata->kind == WIIU_TEXTURE_KIND_P010) {
        size_t size = 0;
        if (!SDL_CalculateYUVSize(texture->format, texture->w, texture->h, &size, NULL)) {
            return false;
        }
        texturedata->shadow_pixels = SDL_calloc(1, size);
        texturedata->shadow_size = size;
        texturedata->shadow_pitch = texture->w * 2;
        return texturedata->shadow_pixels != NULL;
    }

    texturedata->shadow_pitch = texture->w * (int)texturedata->bytes_per_pixel;
    texturedata->shadow_size = (size_t)texturedata->shadow_pitch * texture->h;
    texturedata->shadow_pixels = SDL_calloc(1, texturedata->shadow_size);
    return texturedata->shadow_pixels != NULL;
}

static bool WIIU_EnsureBackbuffer(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    int w = 0;
    int h = 0;

    SDL_GetWindowSizeInPixels(renderer->window, &w, &h);
    w = SDL_max(w, 1);
    h = SDL_max(h, 1);

    if (data->backbuffer.texture.surface.image && data->backbuffer_w == w && data->backbuffer_h == h) {
        return true;
    }

    WIIU_DestroyTextureData(&data->backbuffer);
    if (!WIIU_CreateGX2Texture(&data->backbuffer.texture, w, h, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                               GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A), true) ||
        !WIIU_InitColorBufferFromTexture(&data->backbuffer)) {
        WIIU_DestroyTextureData(&data->backbuffer);
        return false;
    }

    data->backbuffer.kind = WIIU_TEXTURE_KIND_RGBA;
    data->backbuffer.bytes_per_pixel = 4;
    data->backbuffer_w = w;
    data->backbuffer_h = h;
    WIIU_InvalidateCachedState(renderer);
    return true;
}

static GX2ColorBuffer *WIIU_GetCurrentColorBuffer(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;

    if (renderer->target) {
        WIIU_TextureData *texturedata = (WIIU_TextureData *)renderer->target->internal;
        return texturedata->color_buffer_ready ? &texturedata->color_buffer : NULL;
    }

    return data->backbuffer.color_buffer_ready ? &data->backbuffer.color_buffer : NULL;
}

static bool WIIU_ApplyBlendMode(WIIU_RenderData *data, SDL_BlendMode blend)
{
    GX2BlendMode src_color;
    GX2BlendMode dst_color;
    GX2BlendMode src_alpha;
    GX2BlendMode dst_alpha;
    GX2BlendCombineMode op_color;
    GX2BlendCombineMode op_alpha;

    if (blend == SDL_BLENDMODE_NONE) {
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
        GX2SetBlendControl(GX2_RENDER_TARGET_0,
                           GX2_BLEND_MODE_ONE,
                           GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD,
                           FALSE,
                           GX2_BLEND_MODE_ONE,
                           GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD);
        return true;
    }

    if (!WIIU_GetBlendFactor(SDL_GetBlendModeSrcColorFactor(blend), &src_color) ||
        !WIIU_GetBlendFactor(SDL_GetBlendModeDstColorFactor(blend), &dst_color) ||
        !WIIU_GetBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blend), &src_alpha) ||
        !WIIU_GetBlendFactor(SDL_GetBlendModeDstAlphaFactor(blend), &dst_alpha) ||
        !WIIU_GetBlendOp(SDL_GetBlendModeColorOperation(blend), &op_color) ||
        !WIIU_GetBlendOp(SDL_GetBlendModeAlphaOperation(blend), &op_alpha)) {
        return SDL_SetError("Unsupported Wii U blend mode");
    }

    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, src_color, dst_color, op_color, TRUE, src_alpha, dst_alpha, op_alpha);
    return true;
}

static void WIIU_SetBaseRenderState(void)
{
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0,
                       GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD,
                       FALSE,
                       GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetAlphaToMask(FALSE, GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
}

static bool WIIU_EnsureSampler(WIIU_RenderData *data, SDL_ScaleMode scale_mode, SDL_TextureAddressMode address_u, SDL_TextureAddressMode address_v, bool indexed)
{
    GX2TexXYFilterMode filter_mode;

    if (data->sampler_scale_mode == scale_mode &&
        data->sampler_address_u == address_u &&
        data->sampler_address_v == address_v &&
        data->sampler_indexed == indexed) {
        return true;
    }

    switch (scale_mode) {
    case SDL_SCALEMODE_LINEAR:
        filter_mode = indexed ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR;
        break;
    case SDL_SCALEMODE_PIXELART:
        filter_mode = indexed ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR;
        break;
    case SDL_SCALEMODE_NEAREST:
    default:
        filter_mode = GX2_TEX_XY_FILTER_MODE_POINT;
        break;
    }

    GX2InitSamplerXYFilter(&data->sampler, filter_mode, filter_mode, GX2_TEX_ANISO_RATIO_NONE);
    GX2InitSamplerClamping(&data->sampler, WIIU_GetClampMode(address_u), WIIU_GetClampMode(address_v), GX2_TEX_CLAMP_MODE_CLAMP);
    data->sampler_scale_mode = scale_mode;
    data->sampler_address_u = address_u;
    data->sampler_address_v = address_v;
    data->sampler_indexed = indexed;
    return true;
}

static SDL_FColor WIIU_GetCommandColor(const SDL_RenderCommand *cmd)
{
    SDL_FColor color = cmd->data.draw.color;

    color.r *= cmd->data.draw.color_scale;
    color.g *= cmd->data.draw.color_scale;
    color.b *= cmd->data.draw.color_scale;
    return color;
}

static void WIIU_FillVertex(WIIU_Vertex *vertex, float x, float y, const SDL_FColor *color, float u, float v)
{
    vertex->position.x = x;
    vertex->position.y = y;
    vertex->color = *color;
    vertex->tex_coord.x = u;
    vertex->tex_coord.y = v;
}

static void WIIU_EmitQuadVertices(WIIU_Vertex *verts, const SDL_FPoint positions[4], const SDL_FPoint tex_coords[4], const SDL_FColor *color)
{
    static const Uint8 indices[6] = { 0, 1, 2, 0, 2, 3 };
    int i;

    for (i = 0; i < SDL_arraysize(indices); ++i) {
        const Uint8 index = indices[i];
        WIIU_FillVertex(&verts[i], positions[index].x, positions[index].y, color, tex_coords[index].x, tex_coords[index].y);
    }
}

static bool WIIU_SetVertexUniforms(WIIU_RenderData *data, const float *transform)
{
    WIIU_VertexUniformBlock *block = (WIIU_VertexUniformBlock *)WIIU_AllocUniformUpload(data, data->vertex_uniform_block_size);

    if (!block) {
        return false;
    }

    WIIU_StoreUniformFloats(block->transform, transform, SDL_arraysize(block->transform));
    GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK | GX2_INVALIDATE_MODE_CPU, block, data->vertex_uniform_block_size);
    GX2SetVertexUniformBlock(data->vertex_uniform_block->offset, data->vertex_uniform_block_size, block);
    return true;
}

static bool WIIU_SetPixelUniforms(WIIU_RenderData *data, const float *texture_type, const float *texel_size, const float *matrix)
{
    WIIU_PixelUniformBlock *block = (WIIU_PixelUniformBlock *)WIIU_AllocUniformUpload(data, data->pixel_uniform_block_size);

    if (!block) {
        return false;
    }

    WIIU_StoreUniformFloats(block->texture_type, texture_type, SDL_arraysize(block->texture_type));
    WIIU_StoreUniformFloats(block->texel_size, texel_size, SDL_arraysize(block->texel_size));
    WIIU_StoreUniformFloats(block->yuv_offset, matrix + 0, SDL_arraysize(block->yuv_offset));
    WIIU_StoreUniformFloats(block->yuv_r_coeff, matrix + 4, SDL_arraysize(block->yuv_r_coeff));
    WIIU_StoreUniformFloats(block->yuv_g_coeff, matrix + 8, SDL_arraysize(block->yuv_g_coeff));
    WIIU_StoreUniformFloats(block->yuv_b_coeff, matrix + 12, SDL_arraysize(block->yuv_b_coeff));

    GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK | GX2_INVALIDATE_MODE_CPU, block, data->pixel_uniform_block_size);
    GX2SetPixelUniformBlock(data->pixel_uniform_block->offset, data->pixel_uniform_block_size, block);
    return true;
}

static bool WIIU_ApplyDrawState(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    WIIU_TextureData *texturedata = cmd->data.draw.texture ? (WIIU_TextureData *)cmd->data.draw.texture->internal : NULL;
    const SDL_ScaleMode scale_mode = texturedata ? cmd->data.draw.texture_scale_mode : SDL_SCALEMODE_NEAREST;
    SDL_Rect viewport = data->viewport;
    GX2ColorBuffer *color_buffer = WIIU_GetCurrentColorBuffer(renderer);
    float transform[4];
    float texture_type[4] = { WIIU_TEXTURE_TYPE_NONE, 0.0f, 0.0f, 0.0f };
    float texel_size[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float yuv_matrix[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.402f, 0.0f,
        1.0f, -0.344136f, -0.714136f, 0.0f,
        1.0f, 1.772f, 0.0f, 0.0f
    };

    if (!color_buffer) {
        return SDL_SetError("Wii U render target is unavailable");
    }

    if (viewport.w <= 0 || viewport.h <= 0) {
        viewport.x = 0;
        viewport.y = 0;
        viewport.w = (int)color_buffer->surface.width;
        viewport.h = (int)color_buffer->surface.height;
    }

    if (data->viewport_dirty) {
        GX2SetViewport((float)viewport.x, (float)viewport.y, (float)viewport.w, (float)viewport.h, 0.0f, 1.0f);
        data->viewport_dirty = false;
    }

    if (data->clip_dirty) {
        if (data->clip_enabled) {
            GX2SetScissor(viewport.x + data->clip_rect.x, viewport.y + data->clip_rect.y, data->clip_rect.w, data->clip_rect.h);
        } else {
            GX2SetScissor(0, 0, color_buffer->surface.width, color_buffer->surface.height);
        }
        data->clip_dirty = false;
    }

    if (data->blend_dirty || data->blend_mode != cmd->data.draw.blend) {
        if (!WIIU_ApplyBlendMode(data, cmd->data.draw.blend)) {
            return false;
        }
        data->blend_mode = cmd->data.draw.blend;
        data->blend_dirty = false;
    }

    if (!WIIU_EnsureSampler(data,
                            scale_mode,
                            cmd->data.draw.texture_address_mode_u,
                            cmd->data.draw.texture_address_mode_v,
                            texturedata && texturedata->kind == WIIU_TEXTURE_KIND_INDEX8)) {
        return false;
    }

    transform[0] = 2.0f / (float)SDL_max(viewport.w, 1);
    transform[1] = -2.0f / (float)SDL_max(viewport.h, 1);
    transform[2] = -1.0f;
    transform[3] = 1.0f;

    if (texturedata) {
        const float *conversion_matrix = NULL;

        switch (texturedata->kind) {
        case WIIU_TEXTURE_KIND_INDEX8:
            texture_type[0] = WIIU_TEXTURE_TYPE_INDEX8;
            texel_size[0] = 1.0f / (float)SDL_max(cmd->data.draw.texture->w, 1);
            texel_size[1] = 1.0f / (float)SDL_max(cmd->data.draw.texture->h, 1);
            break;
        case WIIU_TEXTURE_KIND_YUV3:
            texture_type[0] = WIIU_TEXTURE_TYPE_YUV3;
            texel_size[0] = 1.0f / (float)SDL_max(cmd->data.draw.texture->w, 1);
            texel_size[1] = 1.0f / (float)SDL_max(cmd->data.draw.texture->h, 1);
            conversion_matrix = SDL_GetYCbCRtoRGBConversionMatrix(cmd->data.draw.texture->colorspace, cmd->data.draw.texture->w, cmd->data.draw.texture->h, 8);
            break;
        case WIIU_TEXTURE_KIND_NV12:
            texture_type[0] = WIIU_TEXTURE_TYPE_NV12;
            texel_size[0] = 1.0f / (float)SDL_max(cmd->data.draw.texture->w, 1);
            texel_size[1] = 1.0f / (float)SDL_max(cmd->data.draw.texture->h, 1);
            conversion_matrix = SDL_GetYCbCRtoRGBConversionMatrix(cmd->data.draw.texture->colorspace, cmd->data.draw.texture->w, cmd->data.draw.texture->h, 8);
            break;
        case WIIU_TEXTURE_KIND_P010:
            texture_type[0] = WIIU_TEXTURE_TYPE_P010;
            texel_size[0] = 1.0f / (float)SDL_max(cmd->data.draw.texture->w, 1);
            texel_size[1] = 1.0f / (float)SDL_max(cmd->data.draw.texture->h, 1);
            conversion_matrix = SDL_GetYCbCRtoRGBConversionMatrix(cmd->data.draw.texture->colorspace, cmd->data.draw.texture->w, cmd->data.draw.texture->h, 10);
            break;
        case WIIU_TEXTURE_KIND_RGBA:
        default:
            texture_type[0] = WIIU_TEXTURE_TYPE_RGBA;
            break;
        }

        if (conversion_matrix) {
            SDL_memcpy(yuv_matrix, conversion_matrix, sizeof(yuv_matrix));
        }
    }

    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetFetchShader(&data->shader.fetch_shader);
    GX2SetVertexShader(data->shader.vertex_shader);
    GX2SetPixelShader(data->shader.pixel_shader);

    if (!WIIU_SetVertexUniforms(data, transform) || !WIIU_SetPixelUniforms(data, texture_type, texel_size, yuv_matrix)) {
        return false;
    }

    if (texturedata) {
        GX2SetPixelTexture(&texturedata->texture, data->sampler0_location);
        GX2SetPixelSampler(&data->sampler, data->sampler0_location);

        if (texturedata->kind == WIIU_TEXTURE_KIND_INDEX8) {
            WIIU_PaletteData *palette = cmd->data.draw.texture->palette ? (WIIU_PaletteData *)cmd->data.draw.texture->palette->internal : NULL;
            if (!palette || !palette->ready) {
                return SDL_SetError("Indexed texture palette is unavailable");
            }
            GX2SetPixelTexture(&palette->texture, data->sampler1_location);
            GX2SetPixelSampler(&data->palette_sampler, data->sampler1_location);
        } else if (texturedata->kind == WIIU_TEXTURE_KIND_YUV3) {
            GX2SetPixelTexture(&texturedata->texture_u, data->sampler1_location);
            GX2SetPixelTexture(&texturedata->texture_v, data->sampler2_location);
            GX2SetPixelSampler(&data->sampler, data->sampler1_location);
            GX2SetPixelSampler(&data->sampler, data->sampler2_location);
        } else if (texturedata->kind == WIIU_TEXTURE_KIND_NV12 || texturedata->kind == WIIU_TEXTURE_KIND_P010) {
            GX2SetPixelTexture(&texturedata->texture_u, data->sampler1_location);
            GX2SetPixelSampler(&data->sampler, data->sampler1_location);
        }
    }

    return true;
}

static bool WIIU_RenderToScanTarget(WIIU_RenderData *data, SDL_WiiUScanTarget *target, GX2ScanTarget scan_target, const SDL_Rect *dstrect)
{
    WIIU_Vertex *vertices;
    const float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float transform[4] = {
        2.0f / (float)SDL_max(dstrect->w, 1),
        -2.0f / (float)SDL_max(dstrect->h, 1),
        -1.0f,
        1.0f
    };
    const float texture_type[4] = { WIIU_TEXTURE_TYPE_RGBA, 0.0f, 0.0f, 0.0f };
    const float texel_size[4] = {
        1.0f / (float)SDL_max(data->backbuffer_w, 1),
        1.0f / (float)SDL_max(data->backbuffer_h, 1),
        0.0f,
        0.0f
    };
    const float yuv_matrix[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    vertices = (WIIU_Vertex *)GX2RLockBufferEx(&data->present_buffer, 0);
    if (!vertices) {
        return SDL_SetError("Unable to lock Wii U present vertex buffer");
    }

    vertices[0].position.x = 0.0f;            vertices[0].position.y = 0.0f;            vertices[0].color.r = color[0]; vertices[0].color.g = color[1]; vertices[0].color.b = color[2]; vertices[0].color.a = color[3]; vertices[0].tex_coord.x = 0.0f; vertices[0].tex_coord.y = 0.0f;
    vertices[1].position.x = (float)dstrect->w; vertices[1].position.y = 0.0f;            vertices[1].color.r = color[0]; vertices[1].color.g = color[1]; vertices[1].color.b = color[2]; vertices[1].color.a = color[3]; vertices[1].tex_coord.x = 1.0f; vertices[1].tex_coord.y = 0.0f;
    vertices[2].position.x = (float)dstrect->w; vertices[2].position.y = (float)dstrect->h; vertices[2].color.r = color[0]; vertices[2].color.g = color[1]; vertices[2].color.b = color[2]; vertices[2].color.a = color[3]; vertices[2].tex_coord.x = 1.0f; vertices[2].tex_coord.y = 1.0f;
    vertices[3].position.x = 0.0f;            vertices[3].position.y = 0.0f;            vertices[3].color.r = color[0]; vertices[3].color.g = color[1]; vertices[3].color.b = color[2]; vertices[3].color.a = color[3]; vertices[3].tex_coord.x = 0.0f; vertices[3].tex_coord.y = 0.0f;
    vertices[4].position.x = (float)dstrect->w; vertices[4].position.y = (float)dstrect->h; vertices[4].color.r = color[0]; vertices[4].color.g = color[1]; vertices[4].color.b = color[2]; vertices[4].color.a = color[3]; vertices[4].tex_coord.x = 1.0f; vertices[4].tex_coord.y = 1.0f;
    vertices[5].position.x = 0.0f;            vertices[5].position.y = (float)dstrect->h; vertices[5].color.r = color[0]; vertices[5].color.g = color[1]; vertices[5].color.b = color[2]; vertices[5].color.a = color[3]; vertices[5].tex_coord.x = 0.0f; vertices[5].tex_coord.y = 1.0f;
    GX2RUnlockBufferEx(&data->present_buffer, 0);

    GX2SetContextState(target->context_state);
    GX2ClearColor(&target->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
    GX2SetContextState(target->context_state);
    GX2SetColorBuffer(&target->color_buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport((float)dstrect->x, (float)dstrect->y, (float)dstrect->w, (float)dstrect->h, 0.0f, 1.0f);
    GX2SetScissor(0, 0, target->render_width, target->render_height);
    WIIU_SetBaseRenderState();
    GX2SetFetchShader(&data->shader.fetch_shader);
    GX2SetVertexShader(data->shader.vertex_shader);
    GX2SetPixelShader(data->shader.pixel_shader);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_TEXTURE | GX2_INVALIDATE_MODE_COLOR_BUFFER),
                  data->backbuffer.color_buffer.surface.image,
                  data->backbuffer.color_buffer.surface.imageSize);
    GX2SetPixelTexture(&data->backbuffer.texture, data->sampler0_location);
    GX2SetPixelSampler(&data->sampler, data->sampler0_location);
    GX2RSetAttributeBuffer(&data->present_buffer, 0, sizeof(WIIU_Vertex), 0);

    if (!WIIU_SetVertexUniforms(data, transform) || !WIIU_SetPixelUniforms(data, texture_type, texel_size, yuv_matrix)) {
        return false;
    }

    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, WIIU_RENDER_PRESENT_VERTEX_COUNT, 0, 1);
    GX2CopyColorBufferToScanBuffer(&target->color_buffer, scan_target);
    return true;
}

static bool WIIU_CreatePalette(SDL_Renderer *renderer, SDL_TexturePalette *palette)
{
    WIIU_PaletteData *palettedata = (WIIU_PaletteData *)SDL_calloc(1, sizeof(*palettedata));
    (void)renderer;

    if (!palettedata) {
        return false;
    }

    if (!WIIU_CreateGX2Texture(&palettedata->texture, 256, 1, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                               GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A), false)) {
        SDL_free(palettedata);
        return false;
    }

    palettedata->ready = true;
    palette->internal = palettedata;
    return true;
}

static bool WIIU_UpdatePalette(SDL_Renderer *renderer, SDL_TexturePalette *palette, int ncolors, SDL_Color *colors)
{
    WIIU_PaletteData *palettedata = (WIIU_PaletteData *)palette->internal;
    Uint8 *pixels;
    int i;
    (void)renderer;

    if (!palettedata || !palettedata->ready) {
        return SDL_SetError("Wii U palette texture is unavailable");
    }

    pixels = (Uint8 *)GX2RLockSurfaceEx(&palettedata->texture.surface, 0, 0);
    if (!pixels) {
        return SDL_SetError("Unable to lock Wii U palette texture");
    }

    SDL_memset(pixels, 0, palettedata->texture.surface.imageSize);
    for (i = 0; i < ncolors && i < 256; ++i) {
        pixels[i * 4 + 0] = colors[i].r;
        pixels[i * 4 + 1] = colors[i].g;
        pixels[i * 4 + 2] = colors[i].b;
        pixels[i * 4 + 3] = colors[i].a;
    }

    GX2RUnlockSurfaceEx(&palettedata->texture.surface, 0, 0);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, palettedata->texture.surface.image, palettedata->texture.surface.imageSize);
    return true;
}

static void WIIU_DestroyPalette(SDL_Renderer *renderer, SDL_TexturePalette *palette)
{
    WIIU_PaletteData *palettedata = (WIIU_PaletteData *)palette->internal;
    (void)renderer;

    if (!palettedata) {
        return;
    }

    WIIU_DestroyGX2Texture(&palettedata->texture);
    SDL_free(palettedata);
    palette->internal = NULL;
}

static bool WIIU_ChangeTexturePalette(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    (void)texture;
    return true;
}

static bool WIIU_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    SDL_GetWindowSizeInPixels(renderer->window, w, h);
    return true;
}

static bool WIIU_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    GX2BlendMode ignored_factor;
    GX2BlendCombineMode ignored_op;
    (void)renderer;

    if (blendMode == SDL_BLENDMODE_NONE) {
        return true;
    }

    return WIIU_GetBlendFactor(SDL_GetBlendModeSrcColorFactor(blendMode), &ignored_factor) &&
           WIIU_GetBlendFactor(SDL_GetBlendModeDstColorFactor(blendMode), &ignored_factor) &&
           WIIU_GetBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blendMode), &ignored_factor) &&
           WIIU_GetBlendFactor(SDL_GetBlendModeDstAlphaFactor(blendMode), &ignored_factor) &&
           WIIU_GetBlendOp(SDL_GetBlendModeColorOperation(blendMode), &ignored_op) &&
           WIIU_GetBlendOp(SDL_GetBlendModeAlphaOperation(blendMode), &ignored_op);
}

static bool WIIU_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)SDL_calloc(1, sizeof(*texturedata));
    const WIIU_FormatInfo *format = WIIU_GetFormatInfo(texture->format);
    (void)renderer;
    (void)create_props;

    if (!texturedata) {
        return false;
    }

    if (format) {
        if (texture->access == SDL_TEXTUREACCESS_TARGET && !format->render_target) {
            SDL_free(texturedata);
            return SDL_SetError("Wii U render targets require RGBA8888 textures");
        }

        if (!WIIU_CreateGX2Texture(&texturedata->texture, texture->w, texture->h, format->surface_format, format->comp_map, texture->access == SDL_TEXTUREACCESS_TARGET)) {
            SDL_free(texturedata);
            return false;
        }

        texturedata->kind = format->kind;
        texturedata->bytes_per_pixel = format->bytes_per_pixel;
        if (texture->access == SDL_TEXTUREACCESS_TARGET) {
            if (!WIIU_InitColorBufferFromTexture(texturedata)) {
                WIIU_DestroyTextureData(texturedata);
                SDL_free(texturedata);
                return false;
            }
        }
    } else if (texture->format == SDL_PIXELFORMAT_INDEX8) {
        if (!WIIU_CreateGX2Texture(&texturedata->texture, texture->w, texture->h, GX2_SURFACE_FORMAT_UNORM_R8,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false)) {
            SDL_free(texturedata);
            return false;
        }
        texturedata->kind = WIIU_TEXTURE_KIND_INDEX8;
        texturedata->bytes_per_pixel = 1;
    } else if (texture->format == SDL_PIXELFORMAT_YV12 || texture->format == SDL_PIXELFORMAT_IYUV) {
        if (!WIIU_CreateGX2Texture(&texturedata->texture, texture->w, texture->h, GX2_SURFACE_FORMAT_UNORM_R8,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false) ||
            !WIIU_CreateGX2Texture(&texturedata->texture_u, (texture->w + 1) / 2, (texture->h + 1) / 2, GX2_SURFACE_FORMAT_UNORM_R8,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false) ||
            !WIIU_CreateGX2Texture(&texturedata->texture_v, (texture->w + 1) / 2, (texture->h + 1) / 2, GX2_SURFACE_FORMAT_UNORM_R8,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false)) {
            WIIU_DestroyTextureData(texturedata);
            SDL_free(texturedata);
            return false;
        }
        texturedata->kind = WIIU_TEXTURE_KIND_YUV3;
        texturedata->bytes_per_pixel = 1;
        texturedata->uv_bytes_per_pixel = 1;
    } else if (texture->format == SDL_PIXELFORMAT_NV12 || texture->format == SDL_PIXELFORMAT_NV21) {
        const uint32_t comp_map = (texture->format == SDL_PIXELFORMAT_NV21)
                                      ? GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_1)
                                      : GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);

        if (!WIIU_CreateGX2Texture(&texturedata->texture, texture->w, texture->h, GX2_SURFACE_FORMAT_UNORM_R8,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false) ||
            !WIIU_CreateGX2Texture(&texturedata->texture_u, (texture->w + 1) / 2, (texture->h + 1) / 2, GX2_SURFACE_FORMAT_UNORM_R8_G8,
                                   comp_map, false)) {
            WIIU_DestroyTextureData(texturedata);
            SDL_free(texturedata);
            return false;
        }
        texturedata->kind = WIIU_TEXTURE_KIND_NV12;
        texturedata->bytes_per_pixel = 1;
        texturedata->uv_bytes_per_pixel = 2;
    } else if (texture->format == SDL_PIXELFORMAT_P010) {
        if (!WIIU_CreateGX2Texture(&texturedata->texture, texture->w, texture->h, GX2_SURFACE_FORMAT_UNORM_R16,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false) ||
            !WIIU_CreateGX2Texture(&texturedata->texture_u, (texture->w + 1) / 2, (texture->h + 1) / 2, GX2_SURFACE_FORMAT_UNORM_R16_G16,
                                   GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1), false)) {
            WIIU_DestroyTextureData(texturedata);
            SDL_free(texturedata);
            return false;
        }
        texturedata->kind = WIIU_TEXTURE_KIND_P010;
        texturedata->bytes_per_pixel = 2;
        texturedata->uv_bytes_per_pixel = 4;
    } else {
        SDL_free(texturedata);
        return SDL_SetError("Unsupported Wii U texture format");
    }

    if (texture->access == SDL_TEXTUREACCESS_STREAMING && !WIIU_EnsureStreamingShadow(texture, texturedata)) {
        WIIU_DestroyTextureData(texturedata);
        SDL_free(texturedata);
        return false;
    }

    texture->internal = texturedata;
    return true;
}

static bool WIIU_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    (void)renderer;

    if (texturedata->kind != WIIU_TEXTURE_KIND_RGBA && texturedata->kind != WIIU_TEXTURE_KIND_INDEX8) {
        return SDL_SetError("Wii U texture format requires planar upload");
    }

    return WIIU_UpdatePlane(&texturedata->texture, rect->x, rect->y, rect->w, rect->h, pixels, pitch, texturedata->bytes_per_pixel);
}

static bool WIIU_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const Uint8 *Yplane, int Ypitch, const Uint8 *Uplane, int Upitch, const Uint8 *Vplane, int Vpitch)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    (void)renderer;

    if (texturedata->kind != WIIU_TEXTURE_KIND_YUV3) {
        return SDL_SetError("Wii U texture is not planar YUV");
    }

    if (!WIIU_UpdatePlane(&texturedata->texture, rect->x, rect->y, rect->w, rect->h, Yplane, Ypitch, 1) ||
        !WIIU_UpdatePlane(&texturedata->texture_u, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, Uplane, Upitch, 1) ||
        !WIIU_UpdatePlane(&texturedata->texture_v, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, Vplane, Vpitch, 1)) {
        return false;
    }

    if (texturedata->shadow_yuv) {
        return SDL_SW_UpdateYUVTexturePlanar(texturedata->shadow_yuv, rect, Yplane, Ypitch, Uplane, Upitch, Vplane, Vpitch);
    }
    return true;
}

static bool WIIU_UpdateTextureNV(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const Uint8 *Yplane, int Ypitch, const Uint8 *UVplane, int UVpitch)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    (void)renderer;

    if (texturedata->kind != WIIU_TEXTURE_KIND_NV12 && texturedata->kind != WIIU_TEXTURE_KIND_P010) {
        return SDL_SetError("Wii U texture is not NV12/NV21/P010");
    }

    if (!WIIU_UpdatePlane(&texturedata->texture, rect->x, rect->y, rect->w, rect->h, Yplane, Ypitch, texturedata->bytes_per_pixel) ||
        !WIIU_UpdatePlane(&texturedata->texture_u, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, UVplane, UVpitch, texturedata->uv_bytes_per_pixel)) {
        return false;
    }

    if (texturedata->shadow_yuv) {
        return SDL_SW_UpdateNVTexturePlanar(texturedata->shadow_yuv, rect, Yplane, Ypitch, UVplane, UVpitch);
    }

    if (texturedata->shadow_pixels) {
        Uint8 *dst_y = (Uint8 *)texturedata->shadow_pixels;
        Uint8 *dst_uv = dst_y + (size_t)texture->w * texture->h * texturedata->bytes_per_pixel;
        int row;

        for (row = 0; row < rect->h; ++row) {
            SDL_memcpy(dst_y + ((rect->y + row) * texturedata->shadow_pitch) + (rect->x * (int)texturedata->bytes_per_pixel),
                       Yplane + (row * Ypitch),
                       (size_t)rect->w * texturedata->bytes_per_pixel);
        }

        for (row = 0; row < (rect->h + 1) / 2; ++row) {
            const int dst_pitch = ((texture->w + 1) / 2) * (int)texturedata->uv_bytes_per_pixel;
            SDL_memcpy(dst_uv + (((rect->y / 2) + row) * dst_pitch) + ((rect->x / 2) * (int)texturedata->uv_bytes_per_pixel),
                       UVplane + (row * UVpitch),
                       (size_t)((rect->w + 1) / 2) * texturedata->uv_bytes_per_pixel);
        }
    }

    return true;
}

static bool WIIU_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    (void)renderer;

    if (!WIIU_EnsureStreamingShadow(texture, texturedata)) {
        return false;
    }

    texturedata->locked = true;
    texturedata->locked_rect = *rect;

    if (texturedata->shadow_yuv) {
        return SDL_SW_LockYUVTexture(texturedata->shadow_yuv, rect, pixels, pitch);
    }

    if (texturedata->kind == WIIU_TEXTURE_KIND_P010) {
        if (rect->x != 0 || rect->y != 0 || rect->w != texture->w || rect->h != texture->h) {
            return SDL_SetError("P010 textures only support full-surface locks");
        }
    }

    *pixels = (Uint8 *)texturedata->shadow_pixels + (rect->y * texturedata->shadow_pitch) + (rect->x * (int)texturedata->bytes_per_pixel);
    *pitch = texturedata->shadow_pitch;
    return true;
}

static void WIIU_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    SDL_Rect rect;
    (void)renderer;

    if (!texturedata || !texturedata->locked) {
        return;
    }

    rect = texturedata->locked_rect;
    texturedata->locked = false;

    if (texturedata->shadow_yuv) {
        void *pixels = NULL;
        int pitch = 0;

        SDL_SW_UnlockYUVTexture(texturedata->shadow_yuv);
        if (SDL_SW_QueryYUVTexturePixels(texturedata->shadow_yuv, &pixels, &pitch)) {
            if (texture->format == SDL_PIXELFORMAT_YV12 || texture->format == SDL_PIXELFORMAT_IYUV) {
                const Uint8 *base = (const Uint8 *)texturedata->shadow_yuv->planes[0];
                const Uint8 *uplane = texturedata->shadow_yuv->planes[1];
                const Uint8 *vplane = texturedata->shadow_yuv->planes[2];
                WIIU_UpdateTextureYUV(renderer, texture, &rect, base, texturedata->shadow_yuv->pitches[0], uplane, texturedata->shadow_yuv->pitches[1], vplane, texturedata->shadow_yuv->pitches[2]);
            } else {
                const Uint8 *base = (const Uint8 *)texturedata->shadow_yuv->planes[0];
                const Uint8 *uvplane = texturedata->shadow_yuv->planes[1];
                WIIU_UpdateTextureNV(renderer, texture, &rect, base, texturedata->shadow_yuv->pitches[0], uvplane, texturedata->shadow_yuv->pitches[1]);
            }
        }
        return;
    }

    if (texturedata->kind == WIIU_TEXTURE_KIND_NV12 || texturedata->kind == WIIU_TEXTURE_KIND_P010) {
        const Uint8 *yplane = (const Uint8 *)texturedata->shadow_pixels;
        const Uint8 *uvplane = yplane + (size_t)texture->w * texture->h * texturedata->bytes_per_pixel;
        const int uv_pitch = ((texture->w + 1) / 2) * (int)texturedata->uv_bytes_per_pixel;
        rect.x = 0;
        rect.y = 0;
        rect.w = texture->w;
        rect.h = texture->h;
        WIIU_UpdateTextureNV(renderer, texture, &rect, yplane, texturedata->shadow_pitch, uvplane, uv_pitch);
        return;
    }

    WIIU_UpdateTexture(renderer, texture, &rect,
                       (Uint8 *)texturedata->shadow_pixels + (rect.y * texturedata->shadow_pitch) + (rect.x * (int)texturedata->bytes_per_pixel),
                       texturedata->shadow_pitch);
}

static bool WIIU_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    (void)texture;

    data->viewport_dirty = true;
    data->clip_dirty = true;
    data->blend_dirty = true;
    return true;
}

static bool WIIU_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    data->viewport = cmd->data.viewport.rect;
    data->viewport_dirty = true;
    return true;
}

static bool WIIU_QueueSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    data->draw_color.r = cmd->data.color.color.r * cmd->data.color.color_scale;
    data->draw_color.g = cmd->data.color.color.g * cmd->data.color.color_scale;
    data->draw_color.b = cmd->data.color.color.b * cmd->data.color.color_scale;
    data->draw_color.a = cmd->data.color.color.a;
    return true;
}

static bool WIIU_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    WIIU_Vertex *verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, (size_t)count * sizeof(*verts), 0, &cmd->data.draw.first);
    int i;

    if (!verts) {
        return false;
    }

    cmd->data.draw.count = count;
    for (i = 0; i < count; ++i) {
        verts[i].position.x = points[i].x + 0.5f;
        verts[i].position.y = points[i].y + 0.5f;
        verts[i].color = data->draw_color;
        verts[i].tex_coord.x = 0.0f;
        verts[i].tex_coord.y = 0.0f;
    }
    return true;
}

static bool WIIU_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    WIIU_Vertex *verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, (size_t)count * sizeof(*verts), 0, &cmd->data.draw.first);
    int i;
    float prevx;
    float prevy;

    if (!verts) {
        return false;
    }

    cmd->data.draw.count = count;
    prevx = points[0].x + 0.5f;
    prevy = points[0].y + 0.5f;
    verts[0].position.x = prevx;
    verts[0].position.y = prevy;
    verts[0].color = data->draw_color;
    verts[0].tex_coord.x = 0.0f;
    verts[0].tex_coord.y = 0.0f;

    for (i = 1; i < count; ++i) {
        const float xend = points[i].x + 0.5f;
        const float yend = points[i].y + 0.5f;
        const float angle = SDL_atan2f(yend - prevy, xend - prevx);
        prevx = xend + (SDL_cosf(angle) * 0.25f);
        prevy = yend + (SDL_sinf(angle) * 0.25f);
        verts[i].position.x = prevx;
        verts[i].position.y = prevy;
        verts[i].color = data->draw_color;
        verts[i].tex_coord.x = 0.0f;
        verts[i].tex_coord.y = 0.0f;
    }
    return true;
}

static bool WIIU_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count)
{
    WIIU_Vertex *verts;
    const SDL_FColor color = WIIU_GetCommandColor(cmd);
    int i;

    verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, (size_t)count * 6 * sizeof(*verts), 0, &cmd->data.draw.first);
    if (!verts) {
        return false;
    }

    cmd->data.draw.count = (size_t)count * 6;
    for (i = 0; i < count; ++i, verts += 6) {
        SDL_FPoint positions[4];
        SDL_FPoint tex_coords[4];

        positions[0].x = rects[i].x;
        positions[0].y = rects[i].y;
        positions[1].x = rects[i].x + rects[i].w;
        positions[1].y = rects[i].y;
        positions[2].x = rects[i].x + rects[i].w;
        positions[2].y = rects[i].y + rects[i].h;
        positions[3].x = rects[i].x;
        positions[3].y = rects[i].y + rects[i].h;

        SDL_zero(tex_coords);
        WIIU_EmitQuadVertices(verts, positions, tex_coords, &color);
    }

    return true;
}

static bool WIIU_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect)
{
    WIIU_Vertex *verts;
    const SDL_FColor color = WIIU_GetCommandColor(cmd);
    const float minu = srcrect->x / texture->w;
    const float minv = srcrect->y / texture->h;
    const float maxu = (srcrect->x + srcrect->w) / texture->w;
    const float maxv = (srcrect->y + srcrect->h) / texture->h;
    SDL_FPoint positions[4];
    SDL_FPoint tex_coords[4];

    verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, 6 * sizeof(*verts), 0, &cmd->data.draw.first);
    if (!verts) {
        return false;
    }

    positions[0].x = dstrect->x;
    positions[0].y = dstrect->y;
    positions[1].x = dstrect->x + dstrect->w;
    positions[1].y = dstrect->y;
    positions[2].x = dstrect->x + dstrect->w;
    positions[2].y = dstrect->y + dstrect->h;
    positions[3].x = dstrect->x;
    positions[3].y = dstrect->y + dstrect->h;

    tex_coords[0].x = minu;
    tex_coords[0].y = minv;
    tex_coords[1].x = maxu;
    tex_coords[1].y = minv;
    tex_coords[2].x = maxu;
    tex_coords[2].y = maxv;
    tex_coords[3].x = minu;
    tex_coords[3].y = maxv;

    cmd->data.draw.count = 6;
    WIIU_EmitQuadVertices(verts, positions, tex_coords, &color);
    return true;
}

static bool WIIU_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect, const double angle, const SDL_FPoint *center, const SDL_FlipMode flip, float scale_x, float scale_y)
{
    WIIU_Vertex *verts;
    const SDL_FColor color = WIIU_GetCommandColor(cmd);
    const float minu = srcrect->x / texture->w;
    const float minv = srcrect->y / texture->h;
    const float maxu = (srcrect->x + srcrect->w) / texture->w;
    const float maxv = (srcrect->y + srcrect->h) / texture->h;
    const float actual_center_x = center ? center->x : (dstrect->w * 0.5f);
    const float actual_center_y = center ? center->y : (dstrect->h * 0.5f);
    const float centerx = actual_center_x + dstrect->x;
    const float centery = actual_center_y + dstrect->y;
    const float radian_angle = (float)((SDL_PI_D * angle) / 180.0);
    const float s = SDL_sinf(radian_angle);
    const float c = SDL_cosf(radian_angle);
    float minx;
    float miny;
    float maxx;
    float maxy;
    float s_minx;
    float s_miny;
    float s_maxx;
    float s_maxy;
    float c_minx;
    float c_miny;
    float c_maxx;
    float c_maxy;
    SDL_FPoint positions[4];
    SDL_FPoint tex_coords[4];

    verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, 6 * sizeof(*verts), 0, &cmd->data.draw.first);
    if (!verts) {
        return false;
    }

    if (flip & SDL_FLIP_HORIZONTAL) {
        minx = dstrect->x + dstrect->w;
        maxx = dstrect->x;
    } else {
        minx = dstrect->x;
        maxx = dstrect->x + dstrect->w;
    }

    if (flip & SDL_FLIP_VERTICAL) {
        miny = dstrect->y + dstrect->h;
        maxy = dstrect->y;
    } else {
        miny = dstrect->y;
        maxy = dstrect->y + dstrect->h;
    }

    tex_coords[0].x = minu;
    tex_coords[0].y = minv;
    tex_coords[1].x = maxu;
    tex_coords[1].y = minv;
    tex_coords[2].x = maxu;
    tex_coords[2].y = maxv;
    tex_coords[3].x = minu;
    tex_coords[3].y = maxv;

    s_minx = s * (minx - centerx);
    s_miny = s * (miny - centery);
    s_maxx = s * (maxx - centerx);
    s_maxy = s * (maxy - centery);
    c_minx = c * (minx - centerx);
    c_miny = c * (miny - centery);
    c_maxx = c * (maxx - centerx);
    c_maxy = c * (maxy - centery);

    positions[0].x = ((c_minx - s_miny) + centerx) * scale_x;
    positions[0].y = ((s_minx + c_miny) + centery) * scale_y;
    positions[1].x = ((c_maxx - s_miny) + centerx) * scale_x;
    positions[1].y = ((s_maxx + c_miny) + centery) * scale_y;
    positions[2].x = ((c_maxx - s_maxy) + centerx) * scale_x;
    positions[2].y = ((s_maxx + c_maxy) + centery) * scale_y;
    positions[3].x = ((c_minx - s_maxy) + centerx) * scale_x;
    positions[3].y = ((s_minx + c_maxy) + centery) * scale_y;

    cmd->data.draw.count = 6;
    WIIU_EmitQuadVertices(verts, positions, tex_coords, &color);
    return true;
}

static bool WIIU_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride, int num_vertices, const void *indices, int num_indices, int size_indices, float scale_x, float scale_y)
{
    WIIU_Vertex *verts;
    const int count = indices ? num_indices : num_vertices;
    const float color_scale = cmd->data.draw.color_scale;
    int i;

    verts = (WIIU_Vertex *)SDL_AllocateRenderVertices(renderer, (size_t)count * sizeof(*verts), 0, &cmd->data.draw.first);
    if (!verts) {
        return false;
    }

    cmd->data.draw.count = count;
    for (i = 0; i < count; ++i) {
        int j;
        const float *xy_;
        SDL_FColor col_;
        const float *uv_;

        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (const float *)((const Uint8 *)xy + (j * xy_stride));
        col_ = *(const SDL_FColor *)((const Uint8 *)color + (j * color_stride));
        uv_ = texture ? (const float *)((const Uint8 *)uv + (j * uv_stride)) : NULL;

        col_.r *= color_scale;
        col_.g *= color_scale;
        col_.b *= color_scale;

        verts[i].position.x = xy_[0] * scale_x;
        verts[i].position.y = xy_[1] * scale_y;
        verts[i].color = col_;
        verts[i].tex_coord.x = uv_ ? uv_[0] : 0.0f;
        verts[i].tex_coord.y = uv_ ? uv_[1] : 0.0f;
    }

    return true;
}

static void WIIU_InvalidateCachedState(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;

    if (!data) {
        return;
    }

    data->viewport.x = 0;
    data->viewport.y = 0;
    data->viewport.w = 0;
    data->viewport.h = 0;
    data->clip_rect.x = 0;
    data->clip_rect.y = 0;
    data->clip_rect.w = 0;
    data->clip_rect.h = 0;
    data->clip_enabled = false;
    data->viewport_dirty = true;
    data->clip_dirty = true;
    data->blend_mode = SDL_BLENDMODE_INVALID;
    data->blend_dirty = true;
    data->sampler_scale_mode = SDL_SCALEMODE_INVALID;
    data->sampler_address_u = SDL_TEXTURE_ADDRESS_INVALID;
    data->sampler_address_v = SDL_TEXTURE_ADDRESS_INVALID;
    data->sampler_indexed = false;
}

static bool WIIU_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    GX2ColorBuffer *color_buffer;

    if (!WIIU_EnsureBackbuffer(renderer)) {
        return false;
    }

    if (renderer->target) {
        WIIU_TextureData *target = (WIIU_TextureData *)renderer->target->internal;
        color_buffer = &target->color_buffer;
    } else {
        color_buffer = &data->backbuffer.color_buffer;
    }

    if (!WIIU_EnsureVertexBuffer(data, vertsize)) {
        return false;
    }

    if (vertsize > 0) {
        void *mapped = GX2RLockBufferEx(&data->vertex_buffer, 0);
        if (!mapped) {
            return SDL_SetError("Unable to lock Wii U vertex buffer");
        }
        SDL_memcpy(mapped, vertices, vertsize);
        GX2RUnlockBufferEx(&data->vertex_buffer, 0);
    }

    GX2SetContextState(data->context_state);
    GX2SetColorBuffer(color_buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)color_buffer->surface.width, (float)color_buffer->surface.height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, color_buffer->surface.width, color_buffer->surface.height);
    WIIU_SetBaseRenderState();
    GX2SetPixelSampler(&data->sampler, data->sampler0_location);

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
            data->viewport = cmd->data.viewport.rect;
            data->viewport_dirty = true;
            break;

        case SDL_RENDERCMD_SETCLIPRECT:
            data->clip_enabled = cmd->data.cliprect.enabled;
            data->clip_rect = cmd->data.cliprect.rect;
            data->clip_dirty = true;
            break;

        case SDL_RENDERCMD_SETDRAWCOLOR:
            break;

        case SDL_RENDERCMD_CLEAR:
            GX2ClearColor(color_buffer,
                          cmd->data.color.color.r * cmd->data.color.color_scale,
                          cmd->data.color.color.g * cmd->data.color.color_scale,
                          cmd->data.color.color.b * cmd->data.color.color_scale,
                          cmd->data.color.color.a);
            GX2SetContextState(data->context_state);
            GX2SetColorBuffer(color_buffer, GX2_RENDER_TARGET_0);
            data->viewport_dirty = true;
            data->clip_dirty = true;
            data->blend_dirty = true;
            WIIU_SetBaseRenderState();
            GX2SetPixelSampler(&data->sampler, data->sampler0_location);
            break;

        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_DRAW_LINES:
        case SDL_RENDERCMD_FILL_RECTS:
        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
        case SDL_RENDERCMD_GEOMETRY:
            if (!WIIU_ApplyDrawState(renderer, cmd)) {
                return false;
            }
            GX2RSetAttributeBuffer(&data->vertex_buffer, 0, sizeof(WIIU_Vertex), (uint32_t)cmd->data.draw.first);
            GX2DrawEx(cmd->command == SDL_RENDERCMD_DRAW_POINTS ? GX2_PRIMITIVE_MODE_POINTS
                                                                : (cmd->command == SDL_RENDERCMD_DRAW_LINES ? GX2_PRIMITIVE_MODE_LINES : GX2_PRIMITIVE_MODE_TRIANGLES),
                      (uint32_t)cmd->data.draw.count, 0, 1);
            break;

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }

    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_TEXTURE | GX2_INVALIDATE_MODE_COLOR_BUFFER),
                  color_buffer->surface.image,
                  color_buffer->surface.imageSize);
    GX2Flush();
    GX2DrawDone();
    return true;
}

static SDL_Surface *WIIU_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    GX2Surface src_surface;
    GX2Texture staging;
    GX2Rect src_rect;
    GX2Point dst_point;
    SDL_Surface *surface;
    Uint8 *src;
    Uint8 *dst;
    int row;
    const int pitch = rect->w * 4;

    if (renderer->target) {
        WIIU_TextureData *texturedata = (WIIU_TextureData *)renderer->target->internal;
        src_surface = texturedata->texture.surface;
    } else {
        src_surface = data->backbuffer.texture.surface;
    }

    surface = SDL_CreateSurface(rect->w, rect->h, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) {
        return NULL;
    }

    if (!WIIU_CreateGX2Texture(&staging, rect->w, rect->h, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                               GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A), false)) {
        SDL_DestroySurface(surface);
        return NULL;
    }

    src_rect.left = rect->x;
    src_rect.top = rect->y;
    src_rect.right = rect->x + rect->w;
    src_rect.bottom = rect->y + rect->h;
    dst_point.x = 0;
    dst_point.y = 0;
    GX2DrawDone();
    GX2CopySurfaceEx(&src_surface, 0, 0, &staging.surface, 0, 0, 1, &src_rect, &dst_point);
    GX2DrawDone();

    src = (Uint8 *)GX2RLockSurfaceEx(&staging.surface, 0, 0);
    if (!src) {
        WIIU_DestroyGX2Texture(&staging);
        SDL_DestroySurface(surface);
        return NULL;
    }

    dst = (Uint8 *)surface->pixels;
    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst + (row * surface->pitch), src + (row * ((int)staging.surface.pitch * 4)), pitch);
    }

    GX2RUnlockSurfaceEx(&staging.surface, 0, 0);
    WIIU_DestroyGX2Texture(&staging);
    return surface;
}

static bool WIIU_RenderPresent(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    SDL_WiiUScanTarget *tv_target = &data->video->tv;
    SDL_WiiUScanTarget *drc_target = &data->video->drc;
    const bool render_tv = SDL_WIIU_GetOutputWindow(data->device, SDL_WIIU_DISPLAY_TV) == renderer->window;
    const bool render_drc = data->video->drc_connected && (SDL_WIIU_GetOutputWindow(data->device, SDL_WIIU_DISPLAY_DRC) == renderer->window);
    const bool mirror_drc_from_tv = data->video->drc_uses_tv_render_buffer && render_tv && render_drc;
    const Uint32 foreground_generation = data->video->foreground_generation;
    const bool first_present_after_acquire = (foreground_generation != 0) && (data->logged_foreground_generation_present != foreground_generation);
    SDL_Rect dstrect;

    if (!SDL_WIIU_IsInForeground()) {
        if (first_present_after_acquire) {
            data->logged_foreground_generation_present = foreground_generation;
            SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                        "WiiU render: first present after foreground acquire #%u skipped because lifecycle still reports background",
                        (unsigned int)foreground_generation);
        }
        return true;
    }

    if (!data->video->foreground_ready) {
        if (first_present_after_acquire) {
            data->logged_foreground_generation_present = foreground_generation;
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                        "WiiU render: first present after foreground acquire #%u skipped because foreground resources are still unavailable",
                        (unsigned int)foreground_generation);
        }
        return true;
    }

    if ((!render_tv || !tv_target->scan_buffer) && (!render_drc || !drc_target->scan_buffer)) {
        if (first_present_after_acquire) {
            data->logged_foreground_generation_present = foreground_generation;
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                        "WiiU render: first present after foreground acquire #%u skipped because no scan targets are active (render_tv=%d, render_drc=%d, tv_buffer=%p, drc_buffer=%p)",
                        (unsigned int)foreground_generation,
                        render_tv ? 1 : 0,
                        render_drc ? 1 : 0,
                        tv_target->scan_buffer,
                        drc_target->scan_buffer);
        }
        return true;
    }

    if (!WIIU_EnsureBackbuffer(renderer)) {
        return false;
    }

    SDL_WIIU_BeginRender();

    if (render_tv && tv_target->scan_buffer) {
        SDL_WIIU_GetPresentationRect(data->backbuffer_w, data->backbuffer_h, (int)tv_target->render_width, (int)tv_target->render_height, &dstrect);
        if (!WIIU_RenderToScanTarget(data, tv_target, GX2_SCAN_TARGET_TV, &dstrect)) {
            return false;
        }
    }

    if (mirror_drc_from_tv && tv_target->scan_buffer && drc_target->scan_buffer) {
        GX2CopyColorBufferToScanBuffer(&tv_target->color_buffer, GX2_SCAN_TARGET_DRC);
    } else if (render_drc && drc_target->scan_buffer) {
        SDL_WIIU_GetPresentationRect(data->backbuffer_w, data->backbuffer_h, (int)drc_target->render_width, (int)drc_target->render_height, &dstrect);
        if (!WIIU_RenderToScanTarget(data, drc_target, GX2_SCAN_TARGET_DRC, &dstrect)) {
            return false;
        }
    }

    GX2SwapScanBuffers();
    GX2Flush();
    GX2DrawDone();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(data->video->drc_connected ? TRUE : FALSE);
    if (first_present_after_acquire) {
        data->logged_foreground_generation_present = foreground_generation;
        SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                    "WiiU render: first present after foreground acquire #%u completed (render_tv=%d, render_drc=%d, tv_buffer=%p, drc_buffer=%p)",
                    (unsigned int)foreground_generation,
                    render_tv ? 1 : 0,
                    render_drc ? 1 : 0,
                    tv_target->scan_buffer,
                    drc_target->scan_buffer);
    }
    return true;
}

static void WIIU_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    WIIU_TextureData *texturedata = (WIIU_TextureData *)texture->internal;
    (void)renderer;

    if (!texturedata) {
        return;
    }

    WIIU_DestroyTextureData(texturedata);
    SDL_free(texturedata);
    texture->internal = NULL;
}

static void WIIU_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    switch (event->type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        WIIU_EnsureBackbuffer(renderer);
        break;
    default:
        break;
    }
}

static bool WIIU_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;

    if (vsync < 0) {
        return SDL_Unsupported();
    }

    data->vsync = vsync ? 1 : 0;
    GX2SetSwapInterval(data->vsync ? 1 : 0);
    return true;
}

static bool WIIU_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    WIIU_RenderData *data = (WIIU_RenderData *)SDL_calloc(1, sizeof(*data));
    int32_t sampler_location;
    SDL_VideoDevice *device = SDL_GetVideoDevice();

    if (!data) {
        return false;
    }

    SDL_SetupRendererColorspace(renderer, create_props);
    if (renderer->output_colorspace != SDL_COLORSPACE_SRGB) {
        SDL_free(data);
        return SDL_SetError("Unsupported output colorspace");
    }

    data->device = device;
    data->video = device ? (SDL_VideoData *)device->internal : NULL;
    if (!data->video) {
        SDL_free(data);
        return SDL_SetError("Wii U video device is unavailable");
    }

    renderer->internal = data;

    data->context_state = MEMAllocFromDefaultHeapEx(sizeof(*data->context_state), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!data->context_state) {
        renderer->internal = NULL;
        SDL_free(data);
        return SDL_SetError("Unable to allocate Wii U GX2 context state");
    }
    GX2SetupContextStateEx(data->context_state, TRUE);

    if (!SDL_WIIU_LoadShaderGroup(&data->shader, SDL_wiiurender_shader_gsh) ||
        !SDL_WIIU_InitShaderAttribute(&data->shader, "a_position", 0, offsetof(WIIU_Vertex, position), GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !SDL_WIIU_InitShaderAttribute(&data->shader, "a_color", 0, offsetof(WIIU_Vertex, color), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32) ||
        !SDL_WIIU_InitShaderAttribute(&data->shader, "a_texcoord", 0, offsetof(WIIU_Vertex, tex_coord), GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !SDL_WIIU_InitFetchShader(&data->shader)) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return false;
    }

    sampler_location = SDL_WIIU_GetPixelSamplerLocation(data->shader.pixel_shader, "u_texture0");
    if (sampler_location < 0) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to resolve Wii U sampler u_texture0");
    }
    data->sampler0_location = (uint32_t)sampler_location;
    sampler_location = SDL_WIIU_GetPixelSamplerLocation(data->shader.pixel_shader, "u_texture1");
    if (sampler_location < 0) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to resolve Wii U sampler u_texture1");
    }
    data->sampler1_location = (uint32_t)sampler_location;
    sampler_location = SDL_WIIU_GetPixelSamplerLocation(data->shader.pixel_shader, "u_texture2");
    if (sampler_location < 0) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to resolve Wii U sampler u_texture2");
    }
    data->sampler2_location = (uint32_t)sampler_location;

    if (data->shader.vertex_shader->mode != GX2_SHADER_MODE_UNIFORM_BLOCK ||
        data->shader.pixel_shader->mode != GX2_SHADER_MODE_UNIFORM_BLOCK) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Wii U renderer shaders must use GX2 uniform blocks");
    }

    data->vertex_uniform_block = GX2GetVertexUniformBlock(data->shader.vertex_shader, "VertexUniforms");
    if (!data->vertex_uniform_block) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to resolve Wii U vertex uniform block VertexUniforms");
    }

    data->pixel_uniform_block = GX2GetPixelUniformBlock(data->shader.pixel_shader, "PixelUniforms");
    if (!data->pixel_uniform_block) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to resolve Wii U pixel uniform block PixelUniforms");
    }

    if (data->vertex_uniform_block->size < sizeof(WIIU_VertexUniformBlock) ||
        data->pixel_uniform_block->size < sizeof(WIIU_PixelUniformBlock)) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Wii U renderer uniform block layout is smaller than expected");
    }

    data->vertex_uniform_block_size = WIIU_AlignUniformBlockSize(data->vertex_uniform_block->size);
    data->pixel_uniform_block_size = WIIU_AlignUniformBlockSize(data->pixel_uniform_block->size);
    if (!WIIU_CreateUniformUploadBuffer(data, WIIU_UNIFORM_UPLOAD_BUFFER_SIZE)) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return false;
    }

    data->present_buffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                 GX2R_RESOURCE_USAGE_CPU_WRITE |
                                 GX2R_RESOURCE_USAGE_GPU_READ |
                                 GX2R_RESOURCE_USAGE_FORCE_MEM2;
    data->present_buffer.elemSize = sizeof(WIIU_Vertex);
    data->present_buffer.elemCount = WIIU_RENDER_PRESENT_VERTEX_COUNT;
    if (!GX2RCreateBuffer(&data->present_buffer)) {
        WIIU_DestroyRenderer((SDL_Renderer *)renderer);
        return SDL_SetError("Unable to allocate Wii U present vertex buffer");
    }

    GX2InitSampler(&data->sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&data->palette_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);

    renderer->WindowEvent = WIIU_WindowEvent;
    renderer->GetOutputSize = WIIU_GetOutputSize;
    renderer->SupportsBlendMode = WIIU_SupportsBlendMode;
    renderer->CreateTexture = WIIU_CreateTexture;
    renderer->UpdateTexture = WIIU_UpdateTexture;
#ifdef SDL_HAVE_YUV
    renderer->UpdateTextureYUV = WIIU_UpdateTextureYUV;
    renderer->UpdateTextureNV = WIIU_UpdateTextureNV;
#endif
    renderer->LockTexture = WIIU_LockTexture;
    renderer->UnlockTexture = WIIU_UnlockTexture;
    renderer->SetRenderTarget = WIIU_SetRenderTarget;
    renderer->QueueSetViewport = WIIU_QueueSetViewport;
    renderer->QueueSetDrawColor = WIIU_QueueSetDrawColor;
    renderer->QueueDrawPoints = WIIU_QueueDrawPoints;
    renderer->QueueDrawLines = WIIU_QueueDrawLines;
    renderer->QueueFillRects = WIIU_QueueFillRects;
    renderer->QueueCopy = WIIU_QueueCopy;
    renderer->QueueCopyEx = WIIU_QueueCopyEx;
    renderer->QueueGeometry = WIIU_QueueGeometry;
    renderer->InvalidateCachedState = WIIU_InvalidateCachedState;
    renderer->RunCommandQueue = WIIU_RunCommandQueue;
    renderer->CreatePalette = WIIU_CreatePalette;
    renderer->UpdatePalette = WIIU_UpdatePalette;
    renderer->DestroyPalette = WIIU_DestroyPalette;
    renderer->ChangeTexturePalette = WIIU_ChangeTexturePalette;
    renderer->RenderReadPixels = WIIU_RenderReadPixels;
    renderer->RenderPresent = WIIU_RenderPresent;
    renderer->DestroyTexture = WIIU_DestroyTexture;
    renderer->DestroyRenderer = WIIU_DestroyRenderer;
    renderer->SetVSync = WIIU_SetVSync;
    renderer->internal = data;
    renderer->window = window;
    renderer->name = WIIU_RenderDriver.name;
    renderer->npot_texture_wrap_unsupported = true;
    renderer->line_method = SDL_RENDERLINEMETHOD_LINES;

    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBX8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XRGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRX8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XBGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGB565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGR565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_INDEX8);
#ifdef SDL_HAVE_YUV
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_YV12);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_IYUV);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_NV12);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_NV21);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_P010);
#endif
    SDL_SetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 1920);

    if (!WIIU_EnsureBackbuffer(renderer) || !WIIU_SetVSync(renderer, 1)) {
        WIIU_DestroyRenderer(renderer);
        return false;
    }

    WIIU_InvalidateCachedState(renderer);
    return true;
}

static void WIIU_DestroyRenderer(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;

    if (!data) {
        return;
    }

    WIIU_DestroyTextureData(&data->backbuffer);
    if (data->present_buffer.buffer) {
        GX2RDestroyBufferEx(&data->present_buffer, 0);
    }
    if (data->vertex_buffer.buffer) {
        GX2RDestroyBufferEx(&data->vertex_buffer, 0);
    }
    if (data->uniform_upload_buffer) {
        MEMFreeToDefaultHeap(data->uniform_upload_buffer);
    }
    SDL_WIIU_DestroyShaderGroup(&data->shader);
    if (data->context_state) {
        MEMFreeToDefaultHeap(data->context_state);
    }
    SDL_free(data);
    renderer->internal = NULL;
}

#endif // SDL_VIDEO_RENDER_WIIU
