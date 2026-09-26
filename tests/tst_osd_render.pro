# In-stream OSD renderer tests (06.6-06, ADR-0045 amended 2026-09-26; ADR-0064).
#
# A pure Qt suite - no SDL, no D3D11, no engine header anywhere in this project. The renderer
# (`app/seathub/osd_renderer.*`) is a `(sizes, colours, strings) -> QImage` transform with no
# dependency on the streaming engine, so this project needs none either.
#
# Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_osd_render.pro && jom && tst_osd_render.exe -o out.txt,txt

QT += core testlib gui

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_osd_render
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

RESOURCES += ../app/seathub/fonts.qrc

# `duration_text.cpp` is the one formatter every client surface uses (FLOW-09); Time left's
# renderer calls it rather than making a second one, so this project has to link it.
SOURCES += tst_osd_render.cpp \
    ../app/seathub/osd_renderer.cpp \
    ../app/seathub/duration_text.cpp

HEADERS += ../app/seathub/osd_renderer.h \
    ../app/seathub/duration_text.h
