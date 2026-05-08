#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "SDL_wiiugfx.h"

#include <coreinit/memdefaultheap.h>
#include <gfd.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/utils.h>

void SDL_WIIU_DestroyShaderGroup(SDL_WiiUShaderGroup *group)
{
    if (!group) {
        return;
    }

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

bool SDL_WIIU_LoadShaderGroup(SDL_WiiUShaderGroup *group, const void *file)
{
    SDL_zero(*group);

    group->vertex_shader = SDL_WIIU_LoadGFDVertexShader(0, file);
    group->pixel_shader = SDL_WIIU_LoadGFDPixelShader(0, file);
    if (!group->vertex_shader || !group->pixel_shader) {
        SDL_WIIU_DestroyShaderGroup(group);
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

int32_t SDL_WIIU_GetPixelSamplerLocation(const GX2PixelShader *shader, const char *name)
{
    uint32_t i;

    for (i = 0; i < shader->samplerVarCount; ++i) {
        if (SDL_strcmp(shader->samplerVars[i].name, name) == 0) {
            return (int32_t)shader->samplerVars[i].location;
        }
    }
    return -1;
}

bool SDL_WIIU_InitShaderAttribute(SDL_WiiUShaderGroup *group, const char *name, uint32_t buffer, uint32_t offset, GX2AttribFormat format)
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

bool SDL_WIIU_InitFetchShader(SDL_WiiUShaderGroup *group)
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

GX2ShaderMode SDL_WIIU_GetShaderMode(const SDL_WiiUShaderGroup *group)
{
    if ((group->vertex_shader && group->vertex_shader->mode == GX2_SHADER_MODE_UNIFORM_BLOCK) ||
        (group->pixel_shader && group->pixel_shader->mode == GX2_SHADER_MODE_UNIFORM_BLOCK)) {
        return GX2_SHADER_MODE_UNIFORM_BLOCK;
    }

    return GX2_SHADER_MODE_UNIFORM_REGISTER;
}

void SDL_WIIU_BeginRender(void)
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

#endif // SDL_VIDEO_DRIVER_WIIU
