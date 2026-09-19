# Control-plane session channel tests (Plan 03-03 Task 1).
#
# Links the QWebSocket client and the HTTP client it shares a base URL and parser with
# (`control_plane_client.cpp`, because `SessionInfo` is one parser for one schema), plus the
# failure model (`error_map.cpp`, D-51). No socket is opened: the routing rules and the backoff
# schedule are asserted directly.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_session_websocket.pro && jom release && tst_session_websocket.exe

QT += core testlib network websockets

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_session_websocket
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_session_websocket.cpp \
    ../app/seathub/session_websocket.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/session_websocket.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h
