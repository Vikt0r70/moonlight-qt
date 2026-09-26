# LogShipper/LogSpool (06.3.1 D-14, D-09, SC-5, Plan 21): SeatHub's own log shipper - a bounded
# in-memory queue and a bounded on-disk spool sitting behind LogTee, with a stub HandOff (never a
# real sentry-native link - that is tst_telemetry.pro's job, and this suite must stay free of
# sentry.h so no other suite that links log_shipper.cpp ever needs the sentry include path).
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
#   cd tests && qmake tst_log_shipper.pro && jom && tst_log_shipper.exe -o tst_log_shipper-out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_log_shipper
DESTDIR = $$OUT_PWD

# tst_log_tee.pro also compiles app/seathub/log_tee.cpp - a private, per-suite object directory
# keeps a naive mtime-based incremental build from ever reusing the other suite's object file
# (tst_telemetry.pro's own comment on OBJECTS_DIR/MOC_DIR explains the same trap).
OBJECTS_DIR = obj-tst_log_shipper
MOC_DIR = obj-tst_log_shipper

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

SOURCES += \
    tst_log_shipper.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/log_shipper.cpp

HEADERS += \
    ../app/seathub/log_tee.h \
    ../app/seathub/log_shipper.h
