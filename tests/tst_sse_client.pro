# SeatHub's SSE frame reader and connection tests (06.4 Plan 14).
#
# Links the control-plane parser (`SessionInfo::parse` - `session.state` reuses it verbatim, one
# parser, never a second one) and the failure model it pulls in transitively, the same way
# `tst_session_websocket.pro` does. Task 1's slots feed `feed()` raw bytes directly, asserting the
# frame parser without a network stack. Task 2 adds `SessionWebSocket::reconnectDelayMs` -
# `reconnectDelayUsesTheScheduleWithJitter` calls it directly - and it is a static defined out of
# line at `session_websocket.cpp:109`, not inline in its header, so `session_websocket.cpp`/`.h`
# join `SOURCES`/`HEADERS` here exactly as `tst_session_websocket.pro` already links them (a
# `Q_OBJECT` class, so moc must see the header or its metaobject will not link) - `websockets`
# joins `QT` for the same reason. `app.pro` already carries both, so the shipped build gains no
# new dependency.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_sse_client.pro && jom release && tst_sse_client.exe

QT += core testlib network websockets

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
    ../app/seathub/error_map.cpp \
    ../app/seathub/session_websocket.cpp

HEADERS += \
    ../app/seathub/sse_client.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h \
    ../app/seathub/session_websocket.h
