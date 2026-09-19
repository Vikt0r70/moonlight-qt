# Liveness and billing-horizon tests (Plan 03-03 Task 2, D-31/D-33/D-34, ADR-0041).
#
# Links both timers, the HTTP client they report through, and the failure model. Nothing waits
# 10 s: the interval, the grace window, the tick and the horizon's fire are all direct, so the
# whole story is deterministic and the run stays in milliseconds.
#
# Build recipe:
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_liveness.pro && jom && tst_liveness.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_liveness
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_liveness.cpp \
    ../app/seathub/liveness_timer.cpp \
    ../app/seathub/authorized_through_timer.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/liveness_timer.h \
    ../app/seathub/authorized_through_timer.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h
