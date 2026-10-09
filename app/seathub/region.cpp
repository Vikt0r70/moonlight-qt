#include "region.h"

#include "countries.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace SeatHubRegion {

QString initialCountryCode()
{
    return SeatHubCountries::defaultIso();
}

} // namespace SeatHubRegion

namespace SeatHubSystem {

bool animationEffectsEnabled()
{
#ifdef Q_OS_WIN
    // SPI_GETCLIENTAREAANIMATION is the setting behind "Animation effects" in Windows 11. The call
    // is looked up at run time so this file adds no link dependency to the test binaries that
    // compile it (the app links user32 anyway).
    using SystemParametersInfoWFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT);
    const HMODULE user32 = ::LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (user32) {
        const auto fn = reinterpret_cast<SystemParametersInfoWFn>(
            ::GetProcAddress(user32, "SystemParametersInfoW"));
        BOOL enabled = TRUE;
        const bool read = fn && fn(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        ::FreeLibrary(user32);
        if (read) {
            return enabled != FALSE;
        }
    }
#endif
    return true;
}

} // namespace SeatHubSystem
