# LogTee's re-entry contract (06.3.1 D-16, area 1 of the design-review trigger, SEATHUB verdict
# F): an addSink()/removeSink() call from inside a sink's own dispatch is refused loudly instead
# of deferred, the in-dispatch flag is reset by an RAII guard, and every sink call is wrapped so
# an exception never crosses SDL's C callback.
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
#   cd tests && qmake tst_log_tee.pro && jom && tst_log_tee.exe -o tst_log_tee-out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_log_tee
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

SOURCES += \
    tst_log_tee.cpp \
    ../app/seathub/log_tee.cpp

HEADERS += \
    ../app/seathub/log_tee.h
