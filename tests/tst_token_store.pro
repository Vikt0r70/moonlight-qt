# DPAPI token-store tests (Plan 03-03 Task 1, D-30).
#
# Runs against the real Windows DPAPI (`crypt32`), which is one of the few production paths
# this environment can exercise for real rather than through a fake. The plaintext check reads
# the bytes that actually landed on disk.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_token_store.pro && jom release && tst_token_store.exe

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_token_store
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

# `CryptProtectData` / `CryptUnprotectData` live in crypt32. The fork's app.pro gains the same
# library for the same reason.
LIBS += -lcrypt32

SOURCES += \
    tst_token_store.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/token_store.h \
    ../app/seathub/error_map.h
