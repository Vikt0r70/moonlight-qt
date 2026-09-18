# HUD bitmap tests (Plan 03-05, ADR-0045).
#
# Task 1 (spike): the pixel-format contract between QImage::Format_ARGB32, an
# SDL_PIXELFORMAT_ARGB8888 surface, and the exact D3D11 texture upload the engine performs.
# The upload half is transcribed (it is private to a renderer class the D-28 boundary forbids
# modifying); the device is WARP, so no GPU, window or swapchain is needed.
#
# Task 2 adds the HUD producer's own tests to the same project, so one build covers both.
#
# Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_hud_bitmap.pro && jom && tst_hud_bitmap.exe -o out.txt,txt

QT += core testlib gui

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_hud_bitmap
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2 -ld3d11 -ldxgi
}

SOURCES += tst_hud_bitmap.cpp
