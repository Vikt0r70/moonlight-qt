# OverlayManager bitmap-injection tests (Plan 03-05 Task 1, ADR-0045).
#
# Links the real `app/streaming/video/overlaymanager.cpp` (the D-28 exception file) plus
# `app/path.cpp`, which its constructor calls through `Path::readDataFile("ModeSeven.ttf")`.
# The `OverlayManager` constructor also calls `TTF_Init()`, hence `-lSDL2_ttf`.
#
# No window, no SDL video subsystem, no D3D11 device and no engine `Session` are needed: the
# injection method's contract is with `IOverlayRenderer`, which the test mocks.
#
# Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_overlay_injection.pro && jom && tst_overlay_injection.exe -o out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_overlay_injection
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2 -lSDL2_ttf
}

SOURCES += \
    tst_overlay_injection.cpp \
    ../app/streaming/video/overlaymanager.cpp \
    ../app/path.cpp

HEADERS += \
    ../app/streaming/video/overlaymanager.h \
    ../app/path.h
