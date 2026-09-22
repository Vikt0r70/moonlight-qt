# QML shell tests (audit F1/F2/F4/E9/E10, D-51 view boundary).
#
# Loads every SeatHub shell screen and asserts the states the UI audit found missing, using
# the technique Plan 03-04 established (`tst_update_feed`): register the generated token
# singletons, instantiate the screen from the filesystem, inject a stand-in facade, then read
# the item tree back. QML has no compiler - a broken binding is invisible to the C++ build and
# only fails when the client starts, which is what this suite exists to catch.
#
# Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_ui_screens.pro && jom release && tst_ui_screens.exe -o out.txt,txt

QT += core testlib gui qml quick quickcontrols2

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_ui_screens
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app $$PWD/../libs/windows/include

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

# Plan 05-11: the Settings-page tests exercise the real `SettingsBridge` (D-24 rebuild), not a
# hand-maintained fake of its whole catalogue surface - the same pairing `tst_facade_wiring.pro`
# and `tst_settings_bridge.pro` already link for the same reason. `app/wm.cpp` comes with it
# because `StreamingPreferences::reload()` consults `WMUtils::isRunningWayland()`.
# `streamingpreferences.h` is listed so qmake runs moc on it: without its own meta-object the
# upstream class links as unresolved externals.
SOURCES += tst_ui_screens.cpp \
    ../app/seathub/agent_config.cpp \
    ../app/seathub/settings_bridge.cpp \
    ../app/settings/streamingpreferences.cpp \
    ../app/wm.cpp

HEADERS += ../app/seathub/agent_config.h \
    ../app/seathub/settings_bridge.h \
    ../app/settings/streamingpreferences.h \
    ../app/utils.h
