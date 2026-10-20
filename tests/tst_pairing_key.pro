# Binary-safe pairing key tests (T090, ADR-0109): the helper `derivePairingKey` that the upstream pair method calls.
#
# Qt Core only (`QT += core testlib`): no SDL, no network, no GUI module. It compiles only the
# production header, so the key derivation is tested through the very code the pair method calls.
#
# Build recipe (Windows; nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_pairing_key.pro && jom && tst_pairing_key.exe -o tst_pairing_key-out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_pairing_key
DESTDIR = $$OUT_PWD

# A private object/moc directory per suite (see tst_attempt_vocab.pro for the trap this avoids).
OBJECTS_DIR = obj-tst_pairing_key
MOC_DIR = obj-tst_pairing_key

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_pairing_key.cpp

HEADERS += \
    ../app/seathub/pairing_key.h
