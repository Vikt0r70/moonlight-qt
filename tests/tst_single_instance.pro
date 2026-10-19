# Single-instance guard tests (T026, ADR-0099, SC20).
#
# Pure Win32 under test (named mutex, named pipe, security descriptors), so no Qt module beyond
# testlib and the Windows libraries the application links.
#
# Build recipe:
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_single_instance.pro && jom && tst_single_instance.exe

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_single_instance
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

LIBS += -ladvapi32 -luser32

SOURCES += \
    tst_single_instance.cpp \
    ../app/seathub/single_instance.cpp

HEADERS += \
    ../app/seathub/single_instance.h
