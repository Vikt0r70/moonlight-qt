# Settings bridge tests (Plan 03-04 Task 1).
#
# Links the bridge's own translation units plus the two upstream files it reads and writes through
# (`app/settings/streamingpreferences.cpp`, and `app/wm.cpp` because `StreamingPreferences::reload()`
# consults `WMUtils::isRunningWayland()`), so the test exercises the one real preference store
# rather than a stand-in.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_settings_bridge.pro && jom release && tst_settings_bridge.exe

QT += core testlib qml quick quickcontrols2 network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_settings_bridge
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app
INCLUDEPATH += $$PWD/../libs/windows/include

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

SOURCES += tst_settings_bridge.cpp \
    ../app/seathub/settings_bridge.cpp \
    ../app/settings/streamingpreferences.cpp \
    ../app/wm.cpp

# `streamingpreferences.h` is listed so qmake runs moc on it: without its own meta-object the
# upstream class links as four unresolved externals.
HEADERS += ../app/seathub/settings_bridge.h \
    ../app/seathub/stats_catalogue.h \
    ../app/settings/streamingpreferences.h \
    ../app/utils.h
