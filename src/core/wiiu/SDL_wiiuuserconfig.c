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

#ifdef SDL_PLATFORM_WIIU

#include "SDL_wiiuuserconfig.h"

#include <coreinit/userconfig.h>

#define SDL_WIIU_LANGUAGE_COUNT 12
#define SDL_WIIU_COUNTRY_US 49u

static const char *const SDL_wiiu_locales[SDL_WIIU_LANGUAGE_COUNT] = {
    "ja_JP",
    "en_GB",
    "fr_FR",
    "de_DE",
    "it_IT",
    "es_ES",
    "zh_CN",
    "ko_KR",
    "nl_NL",
    "pt_PT",
    "ru_RU",
    "zh_TW"
};

bool SDL_WIIU_ReadLocaleValues(Uint32 *language, Uint32 *country)
{
    UCHandle handle;
    UCSysConfig settings[2];
    Uint32 values[2] = { 0, 0 };
    bool have_value = false;

    handle = UCOpen();
    if (handle < 0) {
        return false;
    }

    SDL_zero(settings);

    SDL_strlcpy(settings[0].name, "cafe.language", sizeof(settings[0].name));
    settings[0].dataType = UC_DATATYPE_UNSIGNED_INT;
    settings[0].dataSize = sizeof(values[0]);
    settings[0].data = &values[0];

    SDL_strlcpy(settings[1].name, "cafe.cntry_reg", sizeof(settings[1].name));
    settings[1].dataType = UC_DATATYPE_UNSIGNED_INT;
    settings[1].dataSize = sizeof(values[1]);
    settings[1].data = &values[1];

    if (UCReadSysConfig(handle, 2, settings) == UC_ERROR_OK) {
        if (language && settings[0].error == UC_ERROR_OK) {
            *language = values[0];
            have_value = true;
        }
        if (country && settings[1].error == UC_ERROR_OK) {
            *country = values[1];
            have_value = true;
        }
    }

    (void)UCClose(handle);
    return have_value;
}

bool SDL_WIIU_GetPreferredLocale(char *buf, size_t buflen)
{
    Uint32 language = 0;
    Uint32 country = 0;
    const char *locale;

    if (!buf || buflen == 0) {
        return SDL_InvalidParamError("buf");
    }

    if (!SDL_WIIU_ReadLocaleValues(&language, &country) || language >= SDL_arraysize(SDL_wiiu_locales)) {
        return SDL_SetError("Could not obtain Wii U system locale");
    }

    locale = SDL_wiiu_locales[language];
    if (language == 1 && country == SDL_WIIU_COUNTRY_US) {
        locale = "en_US";
    }

    SDL_strlcpy(buf, locale, buflen);
    return true;
}

#endif // SDL_PLATFORM_WIIU
