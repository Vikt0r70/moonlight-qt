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

INCLUDEPATH += $$PWD/.. $$PWD/../app

DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

SOURCES += tst_ui_screens.cpp \
    ../app/seathub/agent_config.cpp

HEADERS += ../app/seathub/agent_config.h
