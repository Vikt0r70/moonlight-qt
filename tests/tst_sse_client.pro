# SeatHub's SSE frame reader tests (06.4 Plan 14 Task 1).
#
# Links the control-plane parser (`SessionInfo::parse` - `session.state` reuses it verbatim, one
# parser, never a second one) and the failure model it pulls in transitively, the same way
# `tst_session_websocket.pro` does. No socket is opened here: `feed()` is fed raw bytes directly,
# so the frame parser is asserted without a network stack (Task 1). Task 2 extends this file to
# link `session_websocket.cpp`/`.h` for `SseClient::reconnectDelayMs`'s own test.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_sse_client.pro && jom release && tst_sse_client.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_sse_client
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_sse_client.cpp \
    ../app/seathub/sse_client.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/sse_client.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h
