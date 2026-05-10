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

#ifdef SDL_TIME_WIIU

#include "../SDL_time_c.h"
#include "../../core/wiiu/SDL_wiiuuserconfig.h"
#include <coreinit/time.h>

#define SDL_WIIU_LANGUAGE_COUNT 12
#define SDL_WIIU_COUNTRY_US 49u
/* Wii U OSTime counts from 2000-01-01, while SDL_Time is Unix time. */
#define SDL_WIIU_OSTIME_EPOCH_OFFSET_SECONDS 946684800LL

static void civil_from_days(Sint64 days, int *year, int *month, int *day)
{
    days += 719468;
    const Sint64 era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);                            // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     // [0, 399]
    const int y = (int)(yoe) + (int)(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                    // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                          // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;                                  // [1, 31]
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;                                     // [1, 12]

    *year = y + (m <= 2);
    *month = (int)m;
    *day = (int)d;
}

static void SDL_WIIU_NormalizeSecondsAndNanoseconds(SDL_Time ticks, Sint64 *seconds, int *nanoseconds)
{
    Sint64 whole_seconds = ticks / SDL_NS_PER_SECOND;
    int ns = (int)(ticks % SDL_NS_PER_SECOND);

    if (ns < 0) {
        ns += SDL_NS_PER_SECOND;
        --whole_seconds;
    }

    *seconds = whole_seconds;
    *nanoseconds = ns;
}

static bool SDL_WIIU_SystemTicksToSDLTime(OSTime system_ticks, SDL_Time *ticks)
{
    static const Sint64 max_seconds = (SDL_MAX_TIME / SDL_NS_PER_SECOND) - 1;
    static const Sint64 min_seconds = (SDL_MIN_TIME / SDL_NS_PER_SECOND) + 1;
    static const Sint64 max_system_seconds = max_seconds - SDL_WIIU_OSTIME_EPOCH_OFFSET_SECONDS;
    static const Sint64 min_system_seconds = min_seconds - SDL_WIIU_OSTIME_EPOCH_OFFSET_SECONDS;
    const Sint64 frequency = (Sint64)OSTimerClockSpeed;
    Sint64 seconds = system_ticks / frequency;
    Sint64 remainder = system_ticks % frequency;
    Uint64 nanoseconds;

    if (remainder < 0) {
        remainder += frequency;
        --seconds;
    }

    if (seconds >= max_system_seconds) {
        *ticks = SDL_MAX_TIME;
        return true;
    }
    if (seconds <= min_system_seconds) {
        *ticks = SDL_MIN_TIME;
        return true;
    }

    seconds += SDL_WIIU_OSTIME_EPOCH_OFFSET_SECONDS;
    nanoseconds = ((Uint64)remainder * SDL_NS_PER_SECOND) / (Uint64)frequency;
    *ticks = (SDL_Time)(seconds * SDL_NS_PER_SECOND) + (SDL_Time)nanoseconds;
    return true;
}

static bool SDL_WIIU_SDLTimeToSystemTicks(SDL_Time ticks, OSTime *system_ticks)
{
    const Sint64 frequency = (Sint64)OSTimerClockSpeed;
    Sint64 seconds;
    int nanoseconds;

    SDL_WIIU_NormalizeSecondsAndNanoseconds(ticks, &seconds, &nanoseconds);
    seconds -= SDL_WIIU_OSTIME_EPOCH_OFFSET_SECONDS;
    *system_ticks = (OSTime)(seconds * frequency);
    *system_ticks += (OSTime)(((Uint64)nanoseconds * (Uint64)frequency) / SDL_NS_PER_SECOND);
    return true;
}

static void SDL_WIIU_TimeToUTCDateTime(SDL_Time ticks, SDL_DateTime *dt)
{
    Sint64 seconds;
    Sint64 days;
    Sint64 remainder;
    int nanoseconds;

    SDL_WIIU_NormalizeSecondsAndNanoseconds(ticks, &seconds, &nanoseconds);

    days = seconds / SDL_SECONDS_PER_DAY;
    remainder = seconds % SDL_SECONDS_PER_DAY;
    if (remainder < 0) {
        remainder += SDL_SECONDS_PER_DAY;
        --days;
    }

    civil_from_days(days, &dt->year, &dt->month, &dt->day);
    dt->hour = (int)(remainder / 3600);
    remainder -= dt->hour * 3600;
    dt->minute = (int)(remainder / 60);
    remainder -= dt->minute * 60;
    dt->second = (int)remainder;
    dt->nanosecond = nanoseconds;
    dt->utc_offset = 0;
    SDL_CivilToDays(dt->year, dt->month, dt->day, &dt->day_of_week, NULL);
}

void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
    static const SDL_DateFormat WIIU_LANGUAGE_DATE_FORMAT[SDL_WIIU_LANGUAGE_COUNT] = {
        SDL_DATE_FORMAT_YYYYMMDD, // Japanese
        SDL_DATE_FORMAT_DDMMYYYY, // English, override en_US below
        SDL_DATE_FORMAT_DDMMYYYY, // French
        SDL_DATE_FORMAT_DDMMYYYY, // German
        SDL_DATE_FORMAT_DDMMYYYY, // Italian
        SDL_DATE_FORMAT_DDMMYYYY, // Spanish
        SDL_DATE_FORMAT_YYYYMMDD, // Simplified Chinese
        SDL_DATE_FORMAT_YYYYMMDD, // Korean
        SDL_DATE_FORMAT_DDMMYYYY, // Dutch
        SDL_DATE_FORMAT_DDMMYYYY, // Portuguese
        SDL_DATE_FORMAT_DDMMYYYY, // Russian
        SDL_DATE_FORMAT_YYYYMMDD  // Traditional Chinese
    };
    Uint32 language = 0;
    Uint32 country = 0;

    if (!SDL_WIIU_ReadLocaleValues(&language, &country) || language >= SDL_arraysize(WIIU_LANGUAGE_DATE_FORMAT)) {
        return;
    }

    if (df) {
        *df = WIIU_LANGUAGE_DATE_FORMAT[language];
    }
    if (tf) {
        *tf = SDL_TIME_FORMAT_24HR;
    }

    if (language == 1 && country == SDL_WIIU_COUNTRY_US) {
        if (df) {
            *df = SDL_DATE_FORMAT_MMDDYYYY;
        }
        if (tf) {
            *tf = SDL_TIME_FORMAT_12HR;
        }
    }
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    CHECK_PARAM(!ticks) {
        return SDL_InvalidParamError("ticks");
    }

    return SDL_WIIU_SystemTicksToSDLTime(OSGetTime(), ticks);
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    CHECK_PARAM(!dt) {
        return SDL_InvalidParamError("dt");
    }

    if (!localTime) {
        SDL_WIIU_TimeToUTCDateTime(ticks, dt);
        return true;
    }

    {
        OSTime system_ticks;
        OSCalendarTime calendar_time;
        SDL_DateTime local_as_utc;
        SDL_Time local_interpreted_ticks;

        if (!SDL_WIIU_SDLTimeToSystemTicks(ticks, &system_ticks)) {
            return false;
        }

        OSTicksToCalendarTime(system_ticks, &calendar_time);

        dt->year = calendar_time.tm_year;
        dt->month = calendar_time.tm_mon + 1;
        dt->day = calendar_time.tm_mday;
        dt->hour = calendar_time.tm_hour;
        dt->minute = calendar_time.tm_min;
        dt->second = calendar_time.tm_sec;
        dt->nanosecond = (int)(ticks % SDL_NS_PER_SECOND);
        if (dt->nanosecond < 0) {
            dt->nanosecond += SDL_NS_PER_SECOND;
        }
        dt->day_of_week = calendar_time.tm_wday;
        dt->utc_offset = 0;

        local_as_utc = *dt;
        if (!SDL_DateTimeToTime(&local_as_utc, &local_interpreted_ticks)) {
            return false;
        }

        dt->utc_offset = (int)((local_interpreted_ticks - ticks) / SDL_NS_PER_SECOND);
        return true;
    }
}

#endif // SDL_TIME_WIIU
