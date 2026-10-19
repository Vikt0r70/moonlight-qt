#pragma once

// The install-time reporting choice on disk (ADR-0107), in the one format the RentaPC setup window
// (installer/bootstrapper, plain Win32, no Qt) writes and the app reads. Header-only on purpose: the
// bootstrapper includes it directly and the unit suites that compile one of the gated senders need no
// extra source file.
//
// The choice lives in a subkey of the app's per-user settings key, OUTSIDE the Velopack `current`
// folder, so an update (which replaces that folder) never touches it. A real uninstall removes the key
// with the rest of the D-45 state (velopack_updater.cpp), so a reinstall starts with no choice.
//
// Two DWORD values, written in the order NoticeVersion then Choice so an interrupted write can only
// ever leave "no choice", never a half-recorded "on":
//   Choice         1 = send, 2 = do not send; absent or any other number = no choice recorded
//   NoticeVersion  the whole-number notice version the customer saw when they chose
//
// Nothing here is a Velopack hook and nothing here does network work.

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace reporting_consent_store {

// The notice text version this package ships. A higher number than the one stored with the customer's
// choice makes the app ask again (ADR-0107 "Notice version"); the same number never does.
inline constexpr unsigned kCurrentNoticeVersion = 1;

inline constexpr unsigned long kChoiceSend = 1;
inline constexpr unsigned long kChoiceDoNotSend = 2;

struct Record
{
    bool recorded = false; // false: no choice exists yet
    bool send = false;     // meaningful only when recorded
    unsigned noticeVersion = 0;
};

// Overridable so the unit tests and the route proof never touch the real key.
inline std::wstring& keyPathStorage()
{
    static std::wstring path = L"Software\\Seven Hills\\SeatHub\\Reporting";
    return path;
}

inline void setKeyPathForTests(const std::wstring& path)
{
    keyPathStorage() = path;
}

#ifdef _WIN32

inline Record read()
{
    Record record;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPathStorage().c_str(), 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return record;
    }
    auto readDword = [key](const wchar_t* name, DWORD* out) {
        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        DWORD value = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size)
                != ERROR_SUCCESS
            || type != REG_DWORD) {
            return false;
        }
        *out = value;
        return true;
    };
    DWORD choice = 0;
    DWORD version = 0;
    if (readDword(L"Choice", &choice) && (choice == kChoiceSend || choice == kChoiceDoNotSend)) {
        record.recorded = true;
        record.send = choice == kChoiceSend;
        record.noticeVersion = readDword(L"NoticeVersion", &version) ? version : 0;
    }
    RegCloseKey(key);
    return record;
}

inline bool write(bool send, unsigned noticeVersion)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPathStorage().c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS) {
        return false;
    }
    const DWORD version = noticeVersion;
    const DWORD choice = send ? kChoiceSend : kChoiceDoNotSend;
    const bool ok = RegSetValueExW(key, L"NoticeVersion", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&version), sizeof(version))
            == ERROR_SUCCESS
        && RegSetValueExW(key, L"Choice", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&choice),
                          sizeof(choice))
            == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// Tests only: removes the whole subkey so the next read is "no choice".
inline void clear()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, keyPathStorage().c_str());
}

#else // non-Windows builds keep the choice in memory: the app is Windows-only for reporting

inline Record& memoryRecord()
{
    static Record record;
    return record;
}
inline Record read() { return memoryRecord(); }
inline bool write(bool send, unsigned noticeVersion)
{
    memoryRecord() = Record{ true, send, noticeVersion };
    return true;
}
inline void clear() { memoryRecord() = Record{}; }

#endif

// A silent install (ADR-0107 "Default"): records "send" at the current notice version, but only when
// no choice exists yet. A recorded choice - on or off - and its notice version are kept exactly, so a
// silent reinstall or repair can never turn an unticked box back on. Returns true when it wrote.
inline bool recordSilentDefaultIfNoChoice()
{
    if (read().recorded) {
        return false;
    }
    return write(true, kCurrentNoticeVersion);
}

// True when the stored choice was made against an older notice than this package ships, so the app
// must ask again. No choice at all is "ask" too, but that is a different question (read().recorded).
inline bool noticeVersionIsNewer(const Record& record, unsigned packageNoticeVersion)
{
    return record.recorded && record.noticeVersion < packageNoticeVersion;
}

} // namespace reporting_consent_store

// ---------------------------------------------------------------------------------------------------
// The notice window: ONE native Win32 implementation shared by the RentaPC setup window
// (installer/bootstrapper) and the app's in-app ask / re-ask, so the two never drift apart. It is
// compiled only by a translation unit that defines RENTAPC_CONSENT_WINDOW (the bootstrapper and
// reporting_consent.cpp); every other includer of this header sees none of it.
//
// The window shows the notice, a box that starts ticked (or at the previous choice for a re-ask), the
// red warning only while the box is unticked, one link to the data-collection page and two buttons.
// Closing it, Escape or the second button answer nothing; the caller records nothing in that case.
// ---------------------------------------------------------------------------------------------------
#if defined(_WIN32) && defined(RENTAPC_CONSENT_WINDOW)

#include <shellapi.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace reporting_consent_window {

// One user-facing string in both languages. ADR-0043: one non-mirrored LTR layout carries both.
struct Bi
{
    const wchar_t* en;
    const wchar_t* ar;
};

struct Spec
{
    const wchar_t* title = L"";
    const wchar_t* noticeText = L"";        // shown in a read-only scrolling box
    Bi changesHeading{ L"", L"" };          // "What changed"; only drawn when changeCount > 0
    const wchar_t* const* changes = nullptr; // one line each, above the box
    int changeCount = 0;
    bool initialSend = true;                 // the box starts ticked
    Bi checkbox{ L"", L"" };
    Bi warning{ L"", L"" };                  // red, only while the box is unticked
    Bi linkLabel{ L"", L"" };
    const wchar_t* linkUrl = L"";
    Bi primary{ L"", L"" };                  // Install / OK
    bool hasCancel = false;
    Bi cancel{ L"", L"" };
};

struct Result
{
    bool answered = false; // false: window closed / Cancel / Escape
    bool send = false;
};

namespace detail {

enum : int
{
    kIdNotice = 100,
    kIdCheck = 101,
    kIdCheckAr = 102,
    kIdWarnEn = 103,
    kIdWarnAr = 104,
    kIdLink = 105,
    kIdChangesHead = 106,
    kIdChanges = 107,
};

struct State
{
    const Spec* spec = nullptr;
    Result result;
    bool done = false;
    HFONT font = nullptr;
    HFONT linkFont = nullptr;
    HWND check = nullptr;
    HWND warnEn = nullptr;
    HWND warnAr = nullptr;
};

inline std::wstring both(const Bi& text, const wchar_t* separator)
{
    std::wstring joined = text.en;
    if (text.ar && text.ar[0]) {
        joined += separator;
        joined += text.ar;
    }
    return joined;
}

inline HWND makeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                      int w, int h, int id, HFONT font)
{
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    if (child && font) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return child;
}

inline void syncWarning(State* state)
{
    const bool unticked = SendMessageW(state->check, BM_GETCHECK, 0, 0) != BST_CHECKED;
    ShowWindow(state->warnEn, unticked ? SW_SHOW : SW_HIDE);
    ShowWindow(state->warnAr, unticked ? SW_SHOW : SW_HIDE);
}

inline LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        const Spec& spec = *state->spec;

        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        state->font = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW linkFont = metrics.lfMessageFont;
        linkFont.lfUnderline = TRUE;
        state->linkFont = CreateFontIndirectW(&linkFont);

        const int margin = 16;
        const int width = 508;
        int y = margin;
        if (spec.changeCount > 0) {
            makeChild(hwnd, L"STATIC", both(spec.changesHeading, L"  /  ").c_str(), SS_LEFT, margin, y,
                      width, 20, kIdChangesHead, state->font);
            y += 22;
            std::wstring lines;
            for (int i = 0; i < spec.changeCount; ++i) {
                lines += L"• ";
                lines += spec.changes[i];
                lines += L"\r\n";
            }
            const int height = 20 * spec.changeCount + 4;
            makeChild(hwnd, L"STATIC", lines.c_str(), SS_LEFT, margin, y, width, height, kIdChanges,
                      state->font);
            y += height + 8;
        }
        makeChild(hwnd, L"EDIT", spec.noticeText,
                  WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, margin, y, width,
                  spec.changeCount > 0 ? 170 : 230, kIdNotice, state->font);
        y += (spec.changeCount > 0 ? 170 : 230) + 12;

        state->check = makeChild(hwnd, L"BUTTON", spec.checkbox.en, BS_AUTOCHECKBOX | WS_TABSTOP, margin, y,
                                 width, 22, kIdCheck, state->font);
        SendMessageW(state->check, BM_SETCHECK, spec.initialSend ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 22;
        makeChild(hwnd, L"STATIC", spec.checkbox.ar, SS_LEFT, margin + 22, y, width - 22, 20, kIdCheckAr,
                  state->font);
        y += 28;
        state->warnEn = makeChild(hwnd, L"STATIC", spec.warning.en, SS_LEFT, margin, y, width, 20, kIdWarnEn,
                                  state->font);
        y += 20;
        state->warnAr = makeChild(hwnd, L"STATIC", spec.warning.ar, SS_LEFT, margin, y, width, 20, kIdWarnAr,
                                  state->font);
        y += 30;
        makeChild(hwnd, L"STATIC", both(spec.linkLabel, L"  /  ").c_str(), SS_LEFT | SS_NOTIFY | WS_TABSTOP,
                  margin, y, width, 22, kIdLink, state->linkFont);
        y += 34;

        const int buttonW = 150;
        const int buttonH = 30;
        int x = margin + width - buttonW;
        makeChild(hwnd, L"BUTTON", both(spec.primary, L" / ").c_str(), BS_DEFPUSHBUTTON | WS_TABSTOP, x, y,
                  buttonW, buttonH, IDOK, state->font);
        if (spec.hasCancel) {
            x -= buttonW + 8;
            makeChild(hwnd, L"BUTTON", both(spec.cancel, L" / ").c_str(), BS_PUSHBUTTON | WS_TABSTOP, x, y,
                      buttonW, buttonH, IDCANCEL, state->font);
        }
        syncWarning(state);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (id == kIdWarnEn || id == kIdWarnAr) {
            SetTextColor(dc, RGB(0xEF, 0x44, 0x44)); // --destructive (ui.md)
        } else if (id == kIdLink) {
            SetTextColor(dc, RGB(0x1D, 0x4E, 0xD8));
        } else {
            break;
        }
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == kIdCheck && code == BN_CLICKED) {
            syncWarning(state);
        } else if (id == kIdLink && code == STN_CLICKED) {
            ShellExecuteW(hwnd, L"open", state->spec->linkUrl, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDOK) {
            state->result.answered = true;
            state->result.send = SendMessageW(state->check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->done = true;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_SETCURSOR: {
        POINT point;
        GetCursorPos(&point);
        HWND over = WindowFromPoint(point);
        if (over && GetDlgCtrlID(over) == kIdLink) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace detail

// Shows the window and blocks until it is answered or closed. Safe before any Qt object exists.
inline Result show(const Spec& spec)
{
    detail::State state;
    state.spec = &spec;

    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = detail::wndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cls.lpszClassName = L"RentaPCReportingNotice";
    RegisterClassExW(&cls); // a second registration (re-ask after the first) fails harmlessly

    const int clientW = 540;
    const int clientH = (spec.changeCount > 0 ? 170 + 22 + 20 * spec.changeCount + 4 + 8 : 230) + 16 + 12
        + 22 + 28 + 20 + 30 + 34 + 30 + 16;
    RECT frame = { 0, 0, clientW, clientH };
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&frame, style, FALSE);
    const int outerW = frame.right - frame.left;
    const int outerH = frame.bottom - frame.top;
    const int left = (GetSystemMetrics(SM_CXSCREEN) - outerW) / 2;
    const int top = (GetSystemMetrics(SM_CYSCREEN) - outerH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, spec.title, style, left, top, outerW,
                                outerH, nullptr, nullptr, cls.hInstance, &state);
    if (!hwnd) {
        return Result{};
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!state.done || IsWindow(hwnd)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (state.font) {
        DeleteObject(state.font);
    }
    if (state.linkFont) {
        DeleteObject(state.linkFont);
    }
    return state.result;
}

// The words, from docs/spec/copy.md "Setup window" (EN + AR). The notice below is the short notice;
// installer/PRIVACY-NOTICE.txt carries it verbatim as its first paragraph (tst_reporting_consent pins
// that), so the setup window (which shows the whole file) and the in-app ask never say different things.
namespace strings {

inline constexpr const wchar_t* kNoticeEn
    = L"To fix problems, RentaPC sends crash reports and logs to Sentry, a third-party service we use "
      L"to receive them, and sends stream quality numbers and session evidence to RentaPC. They never "
      L"include your PIN, password, email or phone number. You can change this any time in RentaPC "
      L"Settings.";
inline constexpr const wchar_t* kNoticeAr
    = L"لنتمكن من إصلاح المشكلات، يرسل RentaPC تقارير الأعطال والسجلات إلى Sentry، وهي خدمة خارجية "
      L"نستخدمها لاستلامها، ويرسل أرقام جودة البث وأدلة الجلسات إلى RentaPC. لا تتضمن أبدًا رمز PIN أو "
      L"كلمة المرور أو البريد الإلكتروني أو رقم الهاتف. يمكنك تغيير ذلك في أي وقت من إعدادات RentaPC.";

inline constexpr const wchar_t* kUrl = L"https://rentapc.online/data-collection";

inline Spec baseSpec(bool initialSend)
{
    Spec spec;
    spec.title = L"RentaPC setup  /  إعداد RentaPC";
    spec.initialSend = initialSend;
    spec.checkbox = { L"send crash reports, logs and session evidence",
                      L"إرسال تقارير الأعطال والسجلات وأدلة الجلسات" };
    spec.warning = { L"We can't give proper support for errors with this turned off.",
                     L"لا يمكننا تقديم دعم مناسب للأخطاء عند إيقاف هذا الخيار." };
    spec.linkLabel = { L"What we collect", L"ما الذي نجمعه" };
    spec.linkUrl = kUrl;
    spec.changesHeading = { L"What changed", L"ما الذي تغيّر" };
    return spec;
}

} // namespace strings

} // namespace reporting_consent_window

#endif // _WIN32 && RENTAPC_CONSENT_WINDOW
