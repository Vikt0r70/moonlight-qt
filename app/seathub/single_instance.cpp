#include "single_instance.h"

#include <sddl.h>

#include <cstdlib>
#include <cstring>

namespace SingleInstance {

namespace {

// Owns a descriptor built from an SDDL string (ConvertStringSecurityDescriptorToSecurityDescriptorW
// allocates it with LocalAlloc, so it is released with LocalFree).
struct SecurityDescriptor {
    explicit SecurityDescriptor(const std::wstring &sddl)
    {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
            ok = true;
        }
    }
    ~SecurityDescriptor()
    {
        if (sd) {
            LocalFree(sd);
        }
    }
    SecurityDescriptor(const SecurityDescriptor &) = delete;
    SecurityDescriptor &operator=(const SecurityDescriptor &) = delete;

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    bool ok = false;
};

bool writeLine(HANDLE pipe, const std::string &line)
{
    DWORD written = 0;
    return WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)
           && written == line.size();
}

// Reads one '\n'-terminated line of at most kMaxLineBytes (newline included) with a deadline.
// PeekNamedPipe is used so a peer that never finishes a line cannot block this side: a blocking
// ReadFile on a synchronous pipe has no timeout.
bool readLine(HANDLE pipe, int timeoutMs, std::string *line)
{
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        char buf[kMaxLineBytes + 1];
        DWORD peeked = 0;
        if (!PeekNamedPipe(pipe, buf, sizeof(buf), &peeked, nullptr, nullptr)) {
            return false;
        }
        const void *newline = std::memchr(buf, '\n', peeked);
        if (newline) {
            const DWORD length = static_cast<DWORD>(static_cast<const char *>(newline) - buf) + 1;
            if (length > kMaxLineBytes) {
                return false;
            }
            DWORD got = 0;
            if (!ReadFile(pipe, buf, length, &got, nullptr) || got != length) {
                return false;
            }
            line->assign(buf, length - 1);
            return true;
        }
        if (peeked > kMaxLineBytes) {
            return false;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(5);
    }
}

// "<prefix> <digits>" -> the number. Rejects anything else.
bool parseNumberLine(const std::string &line, const char *prefix, unsigned long *out)
{
    const size_t prefixLen = std::strlen(prefix);
    if (line.size() <= prefixLen + 1 || line.compare(0, prefixLen, prefix) != 0 || line[prefixLen] != ' ') {
        return false;
    }
    const std::string digits = line.substr(prefixLen + 1);
    if (digits.size() > 10) {
        return false;
    }
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    *out = std::strtoul(digits.c_str(), nullptr, 10);
    return true;
}

HANDLE openSignalPipe(const std::wstring &pipeName)
{
    // SECURITY_ANONYMOUS: the first copy learns nothing about who connected, and cannot act as them.
    return CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
}

struct FindWindowState {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

BOOL CALLBACK findQtWindow(HWND hwnd, LPARAM param)
{
    auto *state = reinterpret_cast<FindWindowState *>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != state->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t className[64] = {};
    if (GetClassNameW(hwnd, className, 64) == 0 || wcsncmp(className, L"Qt", 2) != 0) {
        return TRUE;
    }
    state->hwnd = hwnd;
    return FALSE;
}

}  // namespace

bool isPlainSid(const std::wstring &sid)
{
    if (sid.size() < 5 || sid.size() > 184 || sid.compare(0, 4, L"S-1-") != 0) {
        return false;
    }
    for (size_t i = 4; i < sid.size(); i++) {
        const wchar_t c = sid[i];
        if (!((c >= L'0' && c <= L'9') || c == L'-')) {
            return false;
        }
    }
    return true;
}

bool currentUserSid(std::wstring *sidOut)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<char> buffer(needed ? needed : 1);
    bool ok = false;
    if (needed && GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
        LPWSTR text = nullptr;
        const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
        if (ConvertSidToStringSidW(user->User.Sid, &text)) {
            sidOut->assign(text);
            LocalFree(text);
            ok = true;
        }
    }
    CloseHandle(token);
    return ok;
}

bool namesFor(const std::wstring &appName, const std::wstring &sid, Names *out)
{
    if (!isPlainSid(sid) || appName.empty()) {
        return false;
    }
    out->lockName = L"Local\\" + appName + L"-" + sid;
    out->pipeName = L"\\\\.\\pipe\\" + appName + L"-signal-" + sid;
    // Owner and SYSTEM only; Medium mandatory label (no-write-up) so a normal and an elevated copy
    // of the same user can both open it.
    out->lockSddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;ME)";
    out->pipeSddl = L"D:P(A;;GRGW;;;" + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;ME)";
    return true;
}

LockResult acquireLock(const Names &names, HANDLE *lockOut)
{
    SecurityDescriptor descriptor(names.lockSddl);
    if (!descriptor.ok) {
        return LockResult::Error;
    }
    HANDLE lock = CreateMutexW(&descriptor.sa, FALSE, names.lockName.c_str());
    const DWORD error = GetLastError();
    if (!lock) {
        // The object exists and this token may not open it: another copy owns it.
        return error == ERROR_ACCESS_DENIED ? LockResult::Existing : LockResult::Error;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        return LockResult::Existing;
    }
    *lockOut = lock;
    return LockResult::Primary;
}

HANDLE createSignalPipe(const Names &names)
{
    SecurityDescriptor descriptor(names.pipeSddl);
    if (!descriptor.ok) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateNamedPipeW(names.pipeName.c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            1, 512, 512, 0, &descriptor.sa);
}

SignalResult signalExisting(const Names &names, const Timing &timing, unsigned *bitsOut)
{
    const ULONGLONG openDeadline = GetTickCount64() + static_cast<ULONGLONG>(timing.pipeOpenWindowMs);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = openSignalPipe(names.pipeName);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        const bool retryable = error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND;
        if (!retryable || GetTickCount64() >= openDeadline) {
            return SignalResult::NotReachable;
        }
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(names.pipeName.c_str(), 50);
        } else {
            Sleep(50);
        }
    }

    SignalResult result = SignalResult::ProtocolError;
    std::string line;
    unsigned long pid = 0;
    unsigned long bits = 0;
    if (readLine(pipe, timing.lineTimeoutMs, &line) && parseNumberLine(line, "PID", &pid)) {
        // The foreground lock only lets the first copy raise its window if we hand the right
        // to it first, so this must come before FOCUS.
        AllowSetForegroundWindow(static_cast<DWORD>(pid));
        if (writeLine(pipe, "FOCUS\n") && readLine(pipe, timing.lineTimeoutMs, &line)
            && parseNumberLine(line, "OK", &bits)) {
            *bitsOut = static_cast<unsigned>(bits);
            result = SignalResult::Answered;
        }
    }
    CloseHandle(pipe);
    return result;
}

SignalServer::~SignalServer()
{
    stop();
}

void SignalServer::start(const Names &names, HANDLE pipe, const Timing &timing, FocusHandler onFocus)
{
    m_names = names;
    m_pipe = pipe;
    m_timing = timing;
    m_onFocus = std::move(onFocus);
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void SignalServer::stop()
{
    if (m_thread.joinable()) {
        m_stop = true;
        // Wake a listener blocked in ConnectNamedPipe with one throwaway connection. A listener that
        // is busy with a client sees the flag at the top of its loop instead.
        const ULONGLONG deadline = GetTickCount64() + 4000;
        while (GetTickCount64() < deadline) {
            HANDLE wake = openSignalPipe(m_names.pipeName);
            if (wake != INVALID_HANDLE_VALUE) {
                CloseHandle(wake);
                break;
            }
            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(m_names.pipeName.c_str(), 100);
            } else {
                break;
            }
        }
        m_thread.join();
    }
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void SignalServer::run()
{
    while (!m_stop) {
        const BOOL connected = ConnectNamedPipe(m_pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            // Every connect error leaves the instance in an unknown state: reset it, pause so a
            // persistent error cannot spin, and listen again.
            DisconnectNamedPipe(m_pipe);
            Sleep(10);
            continue;
        }
        if (m_stop) {
            DisconnectNamedPipe(m_pipe);
            break;
        }

        std::string line;
        if (writeLine(m_pipe, "PID " + std::to_string(GetCurrentProcessId()) + "\n")
            && readLine(m_pipe, m_timing.lineTimeoutMs, &line) && line == "FOCUS") {
            const unsigned bits = m_onFocus ? m_onFocus() : 0u;
            writeLine(m_pipe, "OK " + std::to_string(bits) + "\n");
        }

        // Wait for the peer to close instead of flushing: FlushFileBuffers would block on a client
        // that never reads. A closed peer makes PeekNamedPipe fail; otherwise give up after the
        // line timeout and drop the connection.
        const ULONGLONG closeDeadline = GetTickCount64() + static_cast<ULONGLONG>(m_timing.lineTimeoutMs);
        DWORD peeked = 0;
        while (PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &peeked, nullptr) && GetTickCount64() < closeDeadline) {
            Sleep(5);
        }
        DisconnectNamedPipe(m_pipe);
    }
}

unsigned focusOwnWindow()
{
    FindWindowState state;
    state.pid = GetCurrentProcessId();
    EnumWindows(findQtWindow, reinterpret_cast<LPARAM>(&state));
    if (!state.hwnd) {
        return 0;
    }
    if (IsIconic(state.hwnd)) {
        // Async: a window thread that is busy must not hold the listener past the line timeout.
        ShowWindowAsync(state.hwnd, SW_RESTORE);
        for (int i = 0; i < 30 && IsIconic(state.hwnd); i++) {
            Sleep(10);
        }
    }
    SetForegroundWindow(state.hwnd);
    unsigned bits = 0;
    if (GetForegroundWindow() == state.hwnd) {
        bits |= 1u;
    }
    if (IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd)) {
        bits |= 2u;
    }
    return bits;
}

StartupDecision decideStartup(const std::wstring &appName, const Timing &timing)
{
    StartupDecision decision;
    std::wstring sid;
    if (!currentUserSid(&sid) || !namesFor(appName, sid, &decision.names)) {
        decision.warnings.push_back("single-instance guard unavailable (user SID could not be read); starting without it");
        return decision;
    }

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timing.startupBudgetMs);
    for (;;) {
        HANDLE lock = nullptr;
        switch (acquireLock(decision.names, &lock)) {
        case LockResult::Primary:
            decision.kind = StartupDecision::Kind::Primary;
            decision.lock = lock;
            return decision;
        case LockResult::Error:
            decision.warnings.push_back("single-instance guard unavailable (lock could not be created); starting without it");
            return decision;
        case LockResult::Existing:
            break;
        }

        unsigned bits = 0;
        if (signalExisting(decision.names, timing, &bits) == SignalResult::Answered) {
            decision.kind = StartupDecision::Kind::AnsweredSecond;
            return decision;
        }
        if (GetTickCount64() >= deadline) {
            decision.kind = StartupDecision::Kind::NeverReached;
            return decision;
        }
        Sleep(static_cast<DWORD>(timing.startupRetryMs));
    }
}

Instance::~Instance()
{
    stop();
}

void Instance::adopt(StartupDecision &&decision)
{
    m_decision = std::move(decision);
}

bool Instance::startListener(const Timing &timing, SignalServer::FocusHandler onFocus, std::string *warning)
{
    if (m_decision.kind != StartupDecision::Kind::Primary || m_listening) {
        return true;
    }
    HANDLE pipe = createSignalPipe(m_decision.names);
    if (pipe == INVALID_HANDLE_VALUE) {
        *warning = "single-instance signal pipe could not be created (error "
                   + std::to_string(GetLastError()) + "); a second copy cannot be answered";
        return false;
    }
    m_server.start(m_decision.names, pipe, timing, std::move(onFocus));
    m_listening = true;
    return true;
}

void Instance::stop()
{
    if (m_listening) {
        m_server.stop();
        m_listening = false;
    }
    if (m_decision.lock) {
        CloseHandle(m_decision.lock);
        m_decision.lock = nullptr;
    }
}

}  // namespace SingleInstance
