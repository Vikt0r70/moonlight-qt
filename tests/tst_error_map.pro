# SeatHub fork test project (Plan 03-02 Task 2).
#
# Upstream moonlight-qt ships no test tree; this is the fork's first one. It links the
# SeatHub bridge's own translation units rather than the application, so a mapping-table
# regression is a few-seconds test run instead of a streaming-build cycle.
#
# Build (nothing is on PATH machine-wide — Qt and MSVC are both absolute):
#   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_error_map.pro && jom release && tst_error_map.exe

QT += core testlib qml quick quickcontrols2

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_error_map

# Keep the binary beside its source so `tests\tst_error_map.exe` is the real path.
DESTDIR = $$OUT_PWD

# `../app` so the test can include the bridge as "seathub/error_map.h", the same path the
# bridge's own translation units use.
INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_error_map.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/error_map.h
