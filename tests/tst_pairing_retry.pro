# Pairing retry budget tests (SC18/N18, ADR-0072 item 2 amendment): one retry per Play, shared by the conflict clear and any other non-PAIRED result.
#
# Qt Core only (`QT += core testlib`): no SDL, no Windows API, no network, no GUI module. It builds
# on Linux through tests/linux/run-core-suites.sh as well as on Windows.
#
# Build recipe (Windows; nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_pairing_retry.pro && jom && tst_pairing_retry.exe -o tst_pairing_retry-out.txt,txt
# Linux: bash tests/linux/run-core-suites.sh --provision tst_pairing_retry

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_pairing_retry
DESTDIR = $$OUT_PWD

# A private object/moc directory per suite (see tst_attempt_vocab.pro for the trap this avoids).
OBJECTS_DIR = obj-tst_pairing_retry
MOC_DIR = obj-tst_pairing_retry

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_pairing_retry.cpp

HEADERS += \
    ../app/seathub/pairing_retry.h
