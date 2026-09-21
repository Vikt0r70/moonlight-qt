#include "region.h"

#include <QLocale>

#include "countries.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace SeatHubRegion {

QString pick(const QString& machine, const QString& locale)
{
    const QString candidates[] = { machine.trimmed().toUpper(), locale.trimmed().toUpper() };
    for (const QString& candidate : candidates) {
        if (candidate.size() == 2 && SeatHubCountries::contains(candidate)) {
            return candidate;
        }
    }
    return SeatHubCountries::defaultIso();
}

QString machineRegion()
{
#ifdef Q_OS_WIN
    // `GetUserDefaultGeoName` (Windows 10 2004 and later) answers the user's "Country or region"
    // as an ISO 3166-1 alpha-2 code, or as a UN M.49 number (`001`) for a region that is not a
    // country. It is looked up at run time so a client on an older build still starts: there the
    // answer is simply "none" and the locale's territory is used.
    using GetUserDefaultGeoNameFn = int(WINAPI*)(LPWSTR, int);
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    const auto fn = kernel ? reinterpret_cast<GetUserDefaultGeoNameFn>(
                                 ::GetProcAddress(kernel, "GetUserDefaultGeoName"))
                           : nullptr;
    if (fn) {
        wchar_t name[16] = {};
        const int written = fn(name, static_cast<int>(sizeof(name) / sizeof(name[0])));
        if (written > 0) {
            const QString code = QString::fromWCharArray(name);
            if (code.size() == 2) {
                return code.toUpper();
            }
        }
    }
#endif
    return QString();
}

QString localeRegion()
{
    return QLocale::territoryToCode(QLocale::system().territory());
}

QString initialCountryCode()
{
    return pick(machineRegion(), localeRegion());
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
