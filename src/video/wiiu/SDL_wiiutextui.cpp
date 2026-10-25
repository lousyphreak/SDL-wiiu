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

extern "C" {
#include "SDL_wiiugfx.h"
#include "SDL_wiiuvideo.h"
#include "../../core/wiiu/SDL_wiiudrc.h"
#include "../../core/wiiu/SDL_wiiukpad.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"
#include "../../core/wiiu/SDL_wiiuuserconfig.h"
#include "../../events/SDL_keyboard_c.h"
}

#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <nn/erreula.h>
#include <nn/swkbd.h>

#include <string>

namespace
{

constexpr Uint32 SDL_WIIU_COUNTRY_US = 49u;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define SDL_WIIU_NATIVE_UTF16_ENCODING "UTF-16BE"
#else
#define SDL_WIIU_NATIVE_UTF16_ENCODING "UTF-16LE"
#endif

struct SDL_WiiUTextUIState
{
    bool fs_initialized = false;
    bool fs_client_ready = false;
    bool swkbd_ready = false;
    bool erreula_ready = false;
    FSClient fs_client;
    void *swkbd_work_memory = nullptr;
    void *erreula_work_memory = nullptr;
};

SDL_WiiUTextUIState SDL_wiiu_textui_state;

static size_t SDL_WIIU_UTF16ByteLength(const char16_t *text)
{
    size_t length = 0;

    if (!text) {
        return 0;
    }

    while (text[length] != u'\0') {
        ++length;
    }
    return (length + 1) * sizeof(char16_t);
}

static std::u16string SDL_WIIU_UTF8ToUTF16(const char *text)
{
    char *converted;
    std::u16string result;

    if (!text || !*text) {
        return result;
    }

    converted = SDL_iconv_string(SDL_WIIU_NATIVE_UTF16_ENCODING, "UTF-8", text, SDL_strlen(text) + 1);
    if (!converted) {
        return result;
    }

    result = reinterpret_cast<const char16_t *>(converted);
    SDL_free(converted);
    return result;
}

static char *SDL_WIIU_UTF16ToUTF8(const char16_t *text)
{
    if (!text) {
        return nullptr;
    }

    return SDL_iconv_string("UTF-8", SDL_WIIU_NATIVE_UTF16_ENCODING,
                            reinterpret_cast<const char *>(text),
                            SDL_WIIU_UTF16ByteLength(text));
}

static bool SDL_WIIU_EnsureFSClient(void)
{
    FSStatus fs_status;

    if (!SDL_wiiu_textui_state.fs_initialized) {
        FSInit();
        SDL_wiiu_textui_state.fs_initialized = true;
    }

    if (SDL_wiiu_textui_state.fs_client_ready) {
        return true;
    }

    SDL_zero(SDL_wiiu_textui_state.fs_client);
    fs_status = FSAddClient(&SDL_wiiu_textui_state.fs_client, FS_ERROR_FLAG_NONE);
    if (fs_status != FS_STATUS_OK) {
        return SDL_SetError("FSAddClient failed (%d)", (int)fs_status);
    }

    SDL_wiiu_textui_state.fs_client_ready = true;
    return true;
}

static void SDL_WIIU_GetLanguageRegion(nn::swkbd::LanguageType *swkbd_language,
                                       nn::swkbd::RegionType *swkbd_region,
                                       nn::erreula::LangType *erreula_language,
                                       nn::erreula::RegionType *erreula_region)
{
    Uint32 language = 1;
    Uint32 country = 0;

    (void)SDL_WIIU_ReadLocaleValues(&language, &country);

    switch (language) {
    case 0:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Japanese;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Japan;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Japanese;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Japan;
        }
        break;
    case 2:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::French;
        }
        if (swkbd_region) {
            *swkbd_region = (country == SDL_WIIU_COUNTRY_US) ? nn::swkbd::RegionType::USA : nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::French;
        }
        if (erreula_region) {
            *erreula_region = (country == SDL_WIIU_COUNTRY_US) ? nn::erreula::RegionType::USA : nn::erreula::RegionType::Europe;
        }
        break;
    case 3:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::German;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::German;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Europe;
        }
        break;
    case 4:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Italian;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Italian;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Europe;
        }
        break;
    case 5:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Spanish;
        }
        if (swkbd_region) {
            *swkbd_region = (country == SDL_WIIU_COUNTRY_US) ? nn::swkbd::RegionType::USA : nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Spanish;
        }
        if (erreula_region) {
            *erreula_region = (country == SDL_WIIU_COUNTRY_US) ? nn::erreula::RegionType::USA : nn::erreula::RegionType::Europe;
        }
        break;
    case 6:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::SimplifiedChinese;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::China;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::SimplifiedChinese;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::China;
        }
        break;
    case 7:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Korean;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Korea;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Korean;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Korea;
        }
        break;
    case 8:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Dutch;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Dutch;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Europe;
        }
        break;
    case 9:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Portuguese;
        }
        if (swkbd_region) {
            *swkbd_region = (country == SDL_WIIU_COUNTRY_US) ? nn::swkbd::RegionType::USA : nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Portuguese;
        }
        if (erreula_region) {
            *erreula_region = (country == SDL_WIIU_COUNTRY_US) ? nn::erreula::RegionType::USA : nn::erreula::RegionType::Europe;
        }
        break;
    case 10:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::Russian;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::Russian;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Europe;
        }
        break;
    case 11:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::TraditionalChinese;
        }
        if (swkbd_region) {
            *swkbd_region = nn::swkbd::RegionType::Taiwan;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::TraditionalChinese;
        }
        if (erreula_region) {
            *erreula_region = nn::erreula::RegionType::Taiwan;
        }
        break;
    case 1:
    default:
        if (swkbd_language) {
            *swkbd_language = nn::swkbd::LanguageType::English;
        }
        if (swkbd_region) {
            *swkbd_region = (country == SDL_WIIU_COUNTRY_US) ? nn::swkbd::RegionType::USA : nn::swkbd::RegionType::Europe;
        }
        if (erreula_language) {
            *erreula_language = nn::erreula::LangType::English;
        }
        if (erreula_region) {
            *erreula_region = (country == SDL_WIIU_COUNTRY_US) ? nn::erreula::RegionType::USA : nn::erreula::RegionType::Europe;
        }
        break;
    }
}

static bool SDL_WIIU_EnsureSwkbd(void)
{
    nn::swkbd::CreateArg create_arg;
    nn::swkbd::RegionType region_type;

    if (SDL_wiiu_textui_state.swkbd_ready) {
        return true;
    }

    if (!SDL_WIIU_EnsureFSClient()) {
        return false;
    }

    SDL_wiiu_textui_state.swkbd_work_memory = MEMAllocFromDefaultHeap(nn::swkbd::GetWorkMemorySize(0));
    if (!SDL_wiiu_textui_state.swkbd_work_memory) {
        return SDL_OutOfMemory();
    }

    SDL_WIIU_GetLanguageRegion(nullptr, &region_type, nullptr, nullptr);
    create_arg.workMemory = SDL_wiiu_textui_state.swkbd_work_memory;
    create_arg.regionType = region_type;
    create_arg.fsClient = &SDL_wiiu_textui_state.fs_client;
    if (!nn::swkbd::Create(create_arg)) {
        MEMFreeToDefaultHeap(SDL_wiiu_textui_state.swkbd_work_memory);
        SDL_wiiu_textui_state.swkbd_work_memory = nullptr;
        return SDL_SetError("nn::swkbd::Create failed");
    }

    SDL_wiiu_textui_state.swkbd_ready = true;
    return true;
}

static bool SDL_WIIU_EnsureErreula(void)
{
    nn::erreula::CreateArg create_arg;
    nn::erreula::LangType language_type;
    nn::erreula::RegionType region_type;

    if (SDL_wiiu_textui_state.erreula_ready) {
        return true;
    }

    if (!SDL_WIIU_EnsureFSClient()) {
        return false;
    }

    SDL_wiiu_textui_state.erreula_work_memory = MEMAllocFromDefaultHeap(nn::erreula::GetWorkMemorySize());
    if (!SDL_wiiu_textui_state.erreula_work_memory) {
        return SDL_OutOfMemory();
    }

    SDL_WIIU_GetLanguageRegion(nullptr, nullptr, &language_type, &region_type);
    create_arg.workMemory = SDL_wiiu_textui_state.erreula_work_memory;
    create_arg.region = region_type;
    create_arg.language = language_type;
    create_arg.fsClient = &SDL_wiiu_textui_state.fs_client;
    if (!nn::erreula::Create(create_arg)) {
        MEMFreeToDefaultHeap(SDL_wiiu_textui_state.erreula_work_memory);
        SDL_wiiu_textui_state.erreula_work_memory = nullptr;
        return SDL_SetError("nn::erreula::Create failed");
    }

    SDL_wiiu_textui_state.erreula_ready = true;
    return true;
}

static void SDL_WIIU_FillSwkbdControllerInfo(nn::swkbd::ControllerInfo *controller_info, VPADStatus *vpad_status)
{
    const SDL_WiiUDRCState *drc_state;

    SDL_zero(*controller_info);

    drc_state = SDL_WIIU_GetDRCState();
    if (drc_state && drc_state->has_status && vpad_status) {
        *vpad_status = drc_state->status;
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad_status->tpNormal, &vpad_status->tpNormal);
        controller_info->vpad = vpad_status;
    }

    for (int i = 0; i < 4; ++i) {
        const SDL_WiiUKPADState *kpad_state = SDL_WIIU_GetKPADState(i);
        if (kpad_state && kpad_state->has_status) {
            controller_info->kpad[i] = const_cast<KPADStatus *>(&kpad_state->status);
        }
    }
}

static void SDL_WIIU_FillErreulaControllerInfo(nn::erreula::ControllerInfo *controller_info, VPADStatus *vpad_status)
{
    const SDL_WiiUDRCState *drc_state;

    SDL_zero(*controller_info);

    drc_state = SDL_WIIU_GetDRCState();
    if (drc_state && drc_state->has_status && vpad_status) {
        *vpad_status = drc_state->status;
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad_status->tpNormal, &vpad_status->tpNormal);
        controller_info->vpad = vpad_status;
    }

    for (int i = 0; i < 4; ++i) {
        const SDL_WiiUKPADState *kpad_state = SDL_WIIU_GetKPADState(i);
        if (kpad_state && kpad_state->has_status) {
            controller_info->kpad[i] = &kpad_state->status;
        }
    }
}

static void SDL_WIIU_PrepareScanTarget(SDL_WiiUScanTarget *target)
{
    GX2SetContextState(target->context_state);
    GX2SetColorBuffer(&target->color_buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)target->render_width, (float)target->render_height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, target->render_width, target->render_height);
}

static void SDL_WIIU_PresentModalFrames(SDL_VideoData *driverdata,
                                        void (*draw_tv)(void),
                                        void (*draw_drc)(void))
{
    if (GX2GetSwapInterval() > 0) {
        SDL_WIIU_BeginRender();
    }

    if (driverdata->tv.scan_buffer) {
        if (!SDL_WIIU_EnsureNativeUIRenderBuffer(driverdata, &driverdata->tv)) {
            return;
        }
        SDL_WIIU_PrepareScanTarget(&driverdata->tv);
        draw_tv();
        GX2CopyColorBufferToScanBuffer(&driverdata->tv.color_buffer, GX2_SCAN_TARGET_TV);
    }

    if (driverdata->drc_connected && driverdata->drc.scan_buffer) {
        if (!SDL_WIIU_EnsureNativeUIRenderBuffer(driverdata, &driverdata->drc)) {
            return;
        }
        SDL_WIIU_PrepareScanTarget(&driverdata->drc);
        draw_drc();
        GX2CopyColorBufferToScanBuffer(&driverdata->drc.color_buffer, GX2_SCAN_TARGET_DRC);
    }

    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(driverdata->drc_connected ? TRUE : FALSE);
}

static nn::swkbd::KeyboardMode SDL_WIIU_GetKeyboardMode(SDL_PropertiesID props)
{
    switch (SDL_GetTextInputType(props)) {
    case SDL_TEXTINPUT_TYPE_NUMBER:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE:
        return nn::swkbd::KeyboardMode::Numpad;
    case SDL_TEXTINPUT_TYPE_TEXT_EMAIL:
    case SDL_TEXTINPUT_TYPE_TEXT_USERNAME:
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN:
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE:
        return nn::swkbd::KeyboardMode::Utf8;
    default:
        return nn::swkbd::KeyboardMode::Full;
    }
}

static nn::swkbd::PasswordMode SDL_WIIU_GetPasswordMode(SDL_PropertiesID props)
{
    switch (SDL_GetTextInputType(props)) {
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN:
        return nn::swkbd::PasswordMode::Hide;
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE:
        return nn::swkbd::PasswordMode::Fade;
    default:
        return nn::swkbd::PasswordMode::Clear;
    }
}

static bool SDL_WIIU_RunSoftwareKeyboard(SDL_VideoDevice *_this, SDL_PropertiesID props)
{
    SDL_VideoData *driverdata = static_cast<SDL_VideoData *>(_this->internal);
    nn::swkbd::AppearArg appear_arg;
    nn::swkbd::ControllerInfo controller_info;
    VPADStatus vpad_status;
    nn::swkbd::LanguageType language_type;
    std::u16string title_text = SDL_WIIU_UTF8ToUTF16(SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_TITLE_STRING, ""));
    std::u16string placeholder_text = SDL_WIIU_UTF8ToUTF16(SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_PLACEHOLDER_STRING, ""));
    std::u16string default_text = SDL_WIIU_UTF8ToUTF16(SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_DEFAULT_TEXT_STRING, ""));
    std::u16string ok_text = SDL_WIIU_UTF8ToUTF16("OK");
    bool submit_pressed = false;
    bool cancel_pressed = false;
    char *utf8 = nullptr;

    if (!driverdata->foreground_ready || !SDL_WIIU_IsInForeground()) {
        return SDL_SetError("Wii U foreground graphics are unavailable");
    }

    if (!SDL_WIIU_EnsureSwkbd()) {
        return false;
    }

    SDL_WIIU_GetLanguageRegion(&language_type, nullptr, nullptr, nullptr);
    appear_arg.keyboardArg.configArg.languageType = language_type;
    appear_arg.keyboardArg.configArg.keyboardMode = SDL_WIIU_GetKeyboardMode(props);
    appear_arg.keyboardArg.configArg.okString = ok_text.c_str();
    appear_arg.keyboardArg.configArg.showWordSuggestions = SDL_GetTextInputAutocorrect(props);
    appear_arg.keyboardArg.configArg.disableNewLine = !SDL_GetTextInputMultiline(props);
    appear_arg.keyboardArg.configArg.drawSysWiiPointer = true;
    appear_arg.inputFormArg.initialText = default_text.empty() ? nullptr : default_text.c_str();
    appear_arg.inputFormArg.hintText = !placeholder_text.empty() ? placeholder_text.c_str() : (title_text.empty() ? nullptr : title_text.c_str());
    appear_arg.inputFormArg.maxTextLength = (int)SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_MAX_LENGTH_NUMBER, -1);
    appear_arg.inputFormArg.passwordMode = SDL_WIIU_GetPasswordMode(props);
    appear_arg.inputFormArg.higlightInitialText = true;

    if (!nn::swkbd::AppearInputForm(appear_arg)) {
        return SDL_SetError("nn::swkbd::AppearInputForm failed");
    }

    SDL_SendScreenKeyboardShown();

    while (SDL_WIIU_IsInForeground()) {
        SDL_WIIU_PumpLifecycle();
        SDL_WIIU_BeginDRCUpdateCycle();
        SDL_WIIU_BeginKPADUpdateCycle();
        SDL_WIIU_FillSwkbdControllerInfo(&controller_info, &vpad_status);
        nn::swkbd::Calc(controller_info);

        if (nn::swkbd::IsNeedCalcSubThreadFont()) {
            nn::swkbd::CalcSubThreadFont();
        }
        if (nn::swkbd::IsNeedCalcSubThreadPredict()) {
            nn::swkbd::CalcSubThreadPredict();
        }

        if (!submit_pressed && nn::swkbd::IsDecideOkButton(nullptr)) {
            submit_pressed = true;
            (void)nn::swkbd::DisappearInputForm();
        }
        if (!cancel_pressed && nn::swkbd::IsDecideCancelButton(nullptr)) {
            cancel_pressed = true;
            (void)nn::swkbd::DisappearInputForm();
        }

        SDL_WIIU_PresentModalFrames(driverdata, nn::swkbd::DrawTV, nn::swkbd::DrawDRC);

        if (nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden &&
            nn::swkbd::GetStateKeyboard() == nn::swkbd::State::Hidden) {
            break;
        }
    }

    SDL_SendScreenKeyboardHidden();

    if (!SDL_WIIU_IsInForeground()) {
        return SDL_SetError("Wii U foreground was lost while the software keyboard was open");
    }

    if (!cancel_pressed) {
        utf8 = SDL_WIIU_UTF16ToUTF8(nn::swkbd::GetInputFormString());
        if (utf8 && *utf8) {
            SDL_SendKeyboardText(utf8);
        }
        SDL_free(utf8);
    }

    return true;
}

static bool SDL_WIIU_RunMessageBox(SDL_VideoDevice *_this, const SDL_MessageBoxData *messageboxdata, int *buttonID)
{
    SDL_VideoData *driverdata = static_cast<SDL_VideoData *>(_this->internal);
    nn::erreula::AppearArg appear_arg;
    nn::erreula::ControllerInfo controller_info;
    VPADStatus vpad_status;
    std::u16string title_text = SDL_WIIU_UTF8ToUTF16(messageboxdata->title ? messageboxdata->title : "");
    std::u16string message_text = SDL_WIIU_UTF8ToUTF16(messageboxdata->message ? messageboxdata->message : "");
    std::u16string button0_text;
    std::u16string button1_text;
    int num_buttons = messageboxdata->numbuttons;
    int selected = -1;

    if (!driverdata->foreground_ready || !SDL_WIIU_IsInForeground()) {
        return SDL_SetError("Wii U foreground graphics are unavailable");
    }

    if (num_buttons > 2) {
        return SDL_SetError("Wii U native message boxes support at most 2 buttons");
    }

    if (!SDL_WIIU_EnsureErreula()) {
        return false;
    }

    if (num_buttons <= 0) {
        button0_text = SDL_WIIU_UTF8ToUTF16("OK");
    } else {
        button0_text = SDL_WIIU_UTF8ToUTF16(messageboxdata->buttons[0].text ? messageboxdata->buttons[0].text : "OK");
    }
    if (num_buttons == 2) {
        button1_text = SDL_WIIU_UTF8ToUTF16(messageboxdata->buttons[1].text ? messageboxdata->buttons[1].text : "Cancel");
    }

    appear_arg.errorArg.errorType = (num_buttons == 2) ? nn::erreula::ErrorType::Message2Button : nn::erreula::ErrorType::Message1Button;
    appear_arg.errorArg.renderTarget = nn::erreula::RenderTarget::Both;
    appear_arg.errorArg.controllerType = nn::erreula::ControllerType::DrcGamepad;
    appear_arg.errorArg.errorMessage = message_text.empty() ? nullptr : message_text.c_str();
    appear_arg.errorArg.button1Label = button0_text.c_str();
    appear_arg.errorArg.button2Label = button1_text.empty() ? nullptr : button1_text.c_str();
    appear_arg.errorArg.errorTitle = title_text.empty() ? nullptr : title_text.c_str();
    nn::erreula::AppearErrorViewer(appear_arg);

    while (SDL_WIIU_IsInForeground()) {
        SDL_WIIU_PumpLifecycle();
        SDL_WIIU_BeginDRCUpdateCycle();
        SDL_WIIU_BeginKPADUpdateCycle();
        SDL_WIIU_FillErreulaControllerInfo(&controller_info, &vpad_status);
        nn::erreula::Calc(controller_info);

        if (num_buttons == 2) {
            if (nn::erreula::IsDecideSelectLeftButtonError()) {
                selected = 0;
                nn::erreula::DisappearErrorViewer();
            } else if (nn::erreula::IsDecideSelectRightButtonError()) {
                selected = 1;
                nn::erreula::DisappearErrorViewer();
            }
        } else if (nn::erreula::IsDecideSelectButtonError()) {
            selected = 0;
            nn::erreula::DisappearErrorViewer();
        }

        SDL_WIIU_PresentModalFrames(driverdata, nn::erreula::DrawTV, nn::erreula::DrawDRC);

        if (nn::erreula::GetStateErrorViewer() == nn::erreula::State::Hidden) {
            break;
        }
    }

    if (!SDL_WIIU_IsInForeground()) {
        return SDL_SetError("Wii U foreground was lost while the message box was open");
    }

    if (buttonID) {
        if (num_buttons > 0) {
            *buttonID = (selected >= 0 && selected < num_buttons) ? messageboxdata->buttons[selected].buttonID : messageboxdata->buttons[0].buttonID;
        } else {
            *buttonID = 0;
        }
    }
    return true;
}

} // namespace

extern "C" bool SDL_WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this)
{
    (void)_this;
    return true;
}

extern "C" bool SDL_WIIU_ShowTextInput(SDL_VideoDevice *_this, SDL_PropertiesID props)
{
    if (!_this) {
        return SDL_InvalidParamError("_this");
    }

    return SDL_WIIU_RunSoftwareKeyboard(_this, props);
}

extern "C" bool SDL_WIIU_ShowMessageBox(SDL_VideoDevice *_this, const SDL_MessageBoxData *messageboxdata, int *buttonID)
{
    if (!_this || !messageboxdata) {
        return SDL_InvalidParamError("messageboxdata");
    }

    return SDL_WIIU_RunMessageBox(_this, messageboxdata, buttonID);
}

extern "C" void SDL_WIIU_QuitTextUI(void)
{
    if (SDL_wiiu_textui_state.swkbd_ready) {
        nn::swkbd::Destroy();
        SDL_wiiu_textui_state.swkbd_ready = false;
    }
    if (SDL_wiiu_textui_state.swkbd_work_memory) {
        MEMFreeToDefaultHeap(SDL_wiiu_textui_state.swkbd_work_memory);
        SDL_wiiu_textui_state.swkbd_work_memory = nullptr;
    }

    if (SDL_wiiu_textui_state.erreula_ready) {
        nn::erreula::Destroy();
        SDL_wiiu_textui_state.erreula_ready = false;
    }
    if (SDL_wiiu_textui_state.erreula_work_memory) {
        MEMFreeToDefaultHeap(SDL_wiiu_textui_state.erreula_work_memory);
        SDL_wiiu_textui_state.erreula_work_memory = nullptr;
    }

    if (SDL_wiiu_textui_state.fs_client_ready) {
        (void)FSDelClient(&SDL_wiiu_textui_state.fs_client, FS_ERROR_FLAG_NONE);
        SDL_wiiu_textui_state.fs_client_ready = false;
    }
    if (SDL_wiiu_textui_state.fs_initialized) {
        FSShutdown();
        SDL_wiiu_textui_state.fs_initialized = false;
    }
}
