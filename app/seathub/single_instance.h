#pragma once

// One running copy of the customer app per Windows user (ADR-0099, SC20).
//
// Two objects, both named by the user's SID so two different users on one PC never see each other:
//   lock  Local\<App>-<SID>              a named mutex that is created and never owned or waited on.
//                                        Creating it answers "is there already a copy?": the second
//                                        creator gets ERROR_ALREADY_EXISTS (or ERROR_ACCESS_DENIED).
//   pipe  \\.\pipe\<App>-signal-<SID>    how a second copy asks the first one to come to the front.
//
// Both carry an explicit security descriptor with a Medium mandatory label, so a normal copy and
// an elevated copy of the same user can reach each other (the cross-integrity case, SC20 combos).
//
// Wire protocol, lines of at most 64 bytes, byte-mode pipe:
//   primary -> second  "PID <n>\n"      the primary's process id
//   second  -> primary "FOCUS\n"        sent only AFTER AllowSetForegroundWindow(<n>)
//   primary -> second  "OK <bits>\n"    bit0 = a window of the primary is now foreground,
//                                       bit1 = a window is shown and not minimised
//
// The primary never touches the running session (N20): the focus action is bounded Win32 work on
// the listener thread that only ever acts on a visible Qt window, and while a stream is running the
// Qt window is hidden, so there is nothing for it to restore.
//
// No Qt types are used here so the module runs first in main(), before Qt, logging or Sentry exist.

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace SingleInstance {

// ADR-0099 frozen values. They are parameters only so a test can shorten the waits.
struct Timing {
    int startupBudgetMs = 10000;   // how long a start keeps trying before it gives up (exit code 3)
    int pipeOpenWindowMs = 500;    // how long a second copy retries opening a busy/absent pipe
    int lineTimeoutMs = 3000;      // each side's bound on reading one protocol line
    int startupRetryMs = 100;      // pause between startup attempts
};

constexpr int kExitAnswered = 0;       // a copy was already running and was asked to come forward
constexpr int kExitNeverReached = 3;   // no existing copy could be reached within the budget
constexpr size_t kMaxLineBytes = 64;

struct Names {
    std::wstring lockName;
    std::wstring pipeName;
    std::wstring lockSddl;
    std::wstring pipeSddl;
};

// Only `S-1-` followed by digits and hyphens is embedded into a descriptor string.
bool isPlainSid(const std::wstring &sid);

// The SID of the user this process runs as, as a string. False if it cannot be read.
bool currentUserSid(std::wstring *sidOut);

// Builds the four strings for one app name and user. False when the SID is not plain.
bool namesFor(const std::wstring &appName, const std::wstring &sid, Names *out);

enum class LockResult {
    Primary,    // this copy created the lock and holds the handle
    Existing,   // another copy holds it
    Error,      // the lock could not be set up
};

// Never takes ownership of the mutex and never waits on it. On Primary, *lockOut owns the handle.
LockResult acquireLock(const Names &names, HANDLE *lockOut);

// Creates the single pipe instance (FILE_FLAG_FIRST_PIPE_INSTANCE, remote clients rejected). A
// second creator of the same name fails. INVALID_HANDLE_VALUE on failure.
HANDLE createSignalPipe(const Names &names);

enum class SignalResult {
    Answered,       // the primary acknowledged FOCUS; *bitsOut is set
    NotReachable,   // the pipe could not be opened inside the open window
    ProtocolError,  // opened, but a line was missing, malformed, over 64 bytes or timed out
};

// The second copy's side. Opens the pipe with anonymous impersonation, reads the pid,
// calls AllowSetForegroundWindow(pid), sends FOCUS, reads the ack.
SignalResult signalExisting(const Names &names, const Timing &timing, unsigned *bitsOut);

// The primary's side: accepts one client at a time on `pipe` until stopped.
class SignalServer {
public:
    // Returns the bits for the ack. Runs on the listener thread.
    using FocusHandler = std::function<unsigned()>;

    SignalServer() = default;
    SignalServer(const SignalServer &) = delete;
    SignalServer &operator=(const SignalServer &) = delete;
    ~SignalServer();

    // Takes ownership of `pipe`.
    void start(const Names &names, HANDLE pipe, const Timing &timing, FocusHandler onFocus);
    void stop();

private:
    void run();

    Names m_names;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    Timing m_timing;
    FocusHandler m_onFocus;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

// The default focus action for the app: bring the user's visible Qt window forward and report
// the ack bits. Acts only on a visible top-level window of this process whose class name starts
// with "Qt", so the stream window and a hidden main window are never touched.
unsigned focusOwnWindow();

// What a start decided. Warnings are returned, not logged, because logging does not exist yet.
struct StartupDecision {
    enum class Kind {
        Primary,         // go on starting; `lock` is held
        Unguarded,       // the guard could not be set up: go on starting without one
        AnsweredSecond,  // an existing copy answered: exit with kExitAnswered
        NeverReached,    // gave up inside the budget: exit with kExitNeverReached
    };
    Kind kind = Kind::Unguarded;
    HANDLE lock = nullptr;
    Names names;
    std::vector<std::string> warnings;
};

// Runs the whole startup decision for `appName` (first thing in main).
StartupDecision decideStartup(const std::wstring &appName, const Timing &timing = Timing());

// Owns the lock handle and, once start() is called, the pipe and its listener thread.
class Instance {
public:
    Instance() = default;
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;
    ~Instance();

    void adopt(StartupDecision &&decision);

    // Starts the pipe listener. Called after Sentry and telemetry exist. When the pipe cannot be
    // created the app keeps running and a warning says second copies cannot be answered.
    // Returns false (and fills *warning) in that case.
    bool startListener(const Timing &timing, SignalServer::FocusHandler onFocus, std::string *warning);

    void stop();

private:
    StartupDecision m_decision;
    SignalServer m_server;
    bool m_listening = false;
};

}  // namespace SingleInstance
