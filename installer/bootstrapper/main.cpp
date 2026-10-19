// The RentaPC setup window (ADR-0107): a small native Win32 program that runs BEFORE the unchanged
// Velopack Setup. It is the published RentaPC-Setup.exe; the Velopack Setup that `vpk pack` writes is
// embedded in it byte for byte and started afterwards with the customer's own command line.
//
// What it does, in order:
//   1. A silent run (-s / --silent): records "send" at the current notice version, but only when no
//      choice is recorded yet (a recorded choice and its notice version are never overwritten), then
//      starts the inner Setup. No window.
//   2. An interactive run with a choice already recorded (a reinstall over an install): starts the inner
//      Setup. The window is for the first install only; the choice changes in RentaPC Settings.
//   3. An interactive first install: shows the notice, the box (ticked), the red warning only while it
//      is unticked, the link to the data-collection page, Install and Cancel. Cancel, Escape or closing
//      the window installs nothing and records nothing. Install records the choice, then starts the
//      inner Setup.
//
// The choice is written by app/seathub/reporting_consent_store.h (the same header the app reads it
// with) into HKCU\Software\Seven Hills\SeatHub\Reporting - outside the Velopack `current` folder, and
// nothing here is a Velopack hook. Win32 only, no Qt runtime, no network, no new dependency.

#define RENTAPC_CONSENT_WINDOW
#include "reporting_consent_store.h"

#include <shellapi.h>

#include <string>
#include <vector>

// asInvoker (per-user install, never an elevation prompt) is set in bootstrapper.pro: the linker rejects
// /MANIFESTUAC as an in-source directive.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

namespace {

constexpr int kResourceNotice = 101;
constexpr int kResourceSetup = 102;
constexpr DWORD kExitCancelled = ERROR_CANCELLED; // 1223: the customer closed the window or pressed Cancel
constexpr DWORD kExitInternal = 1;

bool loadResource(int id, const void** data, DWORD* size)
{
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) {
        return false;
    }
    HGLOBAL loaded = LoadResource(nullptr, found);
    if (!loaded) {
        return false;
    }
    *data = LockResource(loaded);
    *size = SizeofResource(nullptr, found);
    return *data != nullptr && *size > 0;
}

// The notice file is UTF-8; an EDIT control wants UTF-16 with CRLF line breaks.
std::wstring noticeText()
{
    const void* data = nullptr;
    DWORD size = 0;
    if (!loadResource(kResourceNotice, &data, &size)) {
        return std::wstring();
    }
    const char* bytes = static_cast<const char*>(data);
    if (size >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes += 3;
        size -= 3;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), &wide[0], length);
    std::wstring crlf;
    crlf.reserve(wide.size() + 64);
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\r') {
            continue;
        }
        if (wide[i] == L'\n') {
            crlf += L"\r\n";
        } else {
            crlf += wide[i];
        }
    }
    return crlf;
}

bool equalsNoCase(const std::wstring& a, const wchar_t* b)
{
    return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

// The command line after the program name, exactly as typed (quoting preserved), so the inner Setup
// receives the customer's arguments unchanged.
std::wstring commandLineTail()
{
    const wchar_t* cursor = GetCommandLineW();
    if (*cursor == L'"') {
        ++cursor;
        while (*cursor && *cursor != L'"') {
            ++cursor;
        }
        if (*cursor == L'"') {
            ++cursor;
        }
    } else {
        while (*cursor && *cursor != L' ' && *cursor != L'\t') {
            ++cursor;
        }
    }
    while (*cursor == L' ' || *cursor == L'\t') {
        ++cursor;
    }
    return cursor;
}

// Writes the embedded Velopack Setup to a private temp folder, byte for byte.
bool extractSetup(std::wstring* folder, std::wstring* file)
{
    const void* data = nullptr;
    DWORD size = 0;
    if (!loadResource(kResourceSetup, &data, &size)) {
        return false;
    }
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (tempLength == 0 || tempLength > MAX_PATH) {
        return false;
    }
    *folder = std::wstring(temp) + L"RentaPC-setup-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(folder->c_str(), nullptr)) {
        return false;
    }
    *file = *folder + L"\\RentaPC-Setup.exe";
    HANDLE handle = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(folder->c_str());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, data, size, &written, nullptr) && written == size;
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(file->c_str());
        RemoveDirectoryW(folder->c_str());
    }
    return ok != FALSE;
}

void removeExtracted(const std::wstring& folder, const std::wstring& file)
{
    // Velopack's Setup may still be releasing its image for a moment after it exits.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            break;
        }
        Sleep(250);
    }
    RemoveDirectoryW(folder.c_str());
}

// Runs the embedded Setup with the customer's arguments and waits for it. Its exit code is ours.
DWORD runInnerSetup(const std::wstring& arguments)
{
    std::wstring folder;
    std::wstring file;
    if (!extractSetup(&folder, &file)) {
        MessageBoxW(nullptr, L"RentaPC setup could not unpack its installer.", L"RentaPC setup",
                    MB_OK | MB_ICONERROR);
        return kExitInternal;
    }
    std::wstring commandLine = L"\"" + file + L"\"";
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    DWORD exitCode = kExitInternal;
    if (CreateProcessW(file.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        MessageBoxW(nullptr, L"RentaPC setup could not start its installer.", L"RentaPC setup",
                    MB_OK | MB_ICONERROR);
    }
    removeExtracted(folder, file);
    return exitCode;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool silent = false;
    std::wstring testKey; // route proof and receipts only: points the store at a throwaway key
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (equalsNoCase(arg, L"-s") || equalsNoCase(arg, L"--silent") || equalsNoCase(arg, L"/s")) {
            silent = true;
        } else if (equalsNoCase(arg, L"--reporting-test-key") && i + 1 < argc) {
            testKey = argv[++i];
        }
    }
    if (argv) {
        LocalFree(argv);
    }

    // Our own test switch is not Velopack's: strip it so the inner Setup sees the customer's line.
    std::wstring tail = commandLineTail();
    if (!testKey.empty()) {
        reporting_consent_store::setKeyPathForTests(testKey);
        const size_t at = tail.find(L"--reporting-test-key");
        if (at != std::wstring::npos) {
            size_t end = at + wcslen(L"--reporting-test-key");
            while (end < tail.size() && tail[end] == L' ') {
                ++end;
            }
            while (end < tail.size() && tail[end] != L' ') {
                ++end;
            }
            while (end < tail.size() && tail[end] == L' ') {
                ++end;
            }
            tail.erase(at, end - at);
        }
    }

    // True when this run wrote a choice that did not exist before, so a failed install can take it back.
    bool wroteFresh = false;
    const reporting_consent_store::Record existing = reporting_consent_store::read();
    if (silent) {
        wroteFresh = reporting_consent_store::recordSilentDefaultIfNoChoice();
    } else if (!existing.recorded) {
        std::wstring notice = noticeText();
        reporting_consent_window::Spec spec = reporting_consent_window::strings::baseSpec(true);
        spec.noticeText = notice.c_str();
        spec.primary = { L"Install", L"تثبيت" };
        spec.hasCancel = true;
        spec.cancel = { L"Cancel", L"إلغاء" };
        const reporting_consent_window::Result answer = reporting_consent_window::show(spec);
        if (!answer.answered) {
            return static_cast<int>(kExitCancelled); // installs nothing, records nothing
        }
        if (!reporting_consent_store::write(answer.send, reporting_consent_store::kCurrentNoticeVersion)) {
            MessageBoxW(nullptr, L"RentaPC setup could not save your choice, so it did not install.",
                        L"RentaPC setup", MB_OK | MB_ICONERROR);
            return static_cast<int>(kExitInternal);
        }
        wroteFresh = true;
    }

    const DWORD exitCode = runInnerSetup(tail);
    if (exitCode != 0 && wroteFresh) {
        reporting_consent_store::clear(); // a failed install leaves no choice behind
    }
    return static_cast<int>(exitCode);
}
