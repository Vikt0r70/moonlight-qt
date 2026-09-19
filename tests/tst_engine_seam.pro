# Engine-seam tests (Plan 03-06 gap closure).
#
# The phase verifier's first blocker was that nothing ever attached a real engine `Session`:
# `attachSession()` had no call site, so `m_session` was always null, `start()` always ran the
# Plan 03-02 tracer, `connectEngineSignals()` was unreachable and `publishHudSurface()` always
# found `Session::get() == nullptr`. Those are properties of the wiring, and the wiring is
# assertable without a host, a Sunshine, or an engine — which is why `app/seathub/engine_session.h`
# exists. This suite injects a fake `EngineSession` and asserts:
#
#   1. With nothing attached, `start()` fails closed. It does NOT fall back to a fake.
#   2. All eight engine signals reach `SessionLifecycle`'s own signals, with their arguments, when
#      they are emitted by an attached session.
#   3. `start()` drives the attached session and `interrupt()` reaches it — the two directions the
#      lifecycle exists to carry.
#   4. `readyForDeletion()` releases the lifecycle (window restore, `active` false, detached) and
#      does NOT destroy the session: the facade owns it, and it is still inside the engine's own
#      frames at that moment.
#   5. `ProductionPairingSeam` announces the resolved host before it reports success, which is what
#      lets `SeatHubClient::handlePairingCompleted()` find an engine session already attached.
#
# No SDL, no engine and no network are linked: the seam's surface type is only ever a pointer here.
#
# Build recipe:
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_engine_seam.pro && jom && tst_engine_seam.exe

QT += core testlib gui network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_engine_seam
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_engine_seam.cpp \
    ../app/seathub/engine_session.cpp \
    ../app/seathub/session_lifecycle.cpp \
    ../app/seathub/pairing_seam.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/engine_session.h \
    ../app/seathub/session_lifecycle.h \
    ../app/seathub/teardown_guard.h \
    ../app/seathub/pairing_seam.h \
    ../app/seathub/error_map.h
