# Attempt vocabulary tests (ADR-0072 item 1, Plan 09 Task 1).
#
# The 11 frozen `attempt_step` tokens and the class mappers behind the redaction rule: a class is
# computed from an enum integer, an HTTP bucket or a stage index and from nothing else. The
# module under test is sentry-free and network-free by construction, so this suite needs no Qt
# module beyond core/testlib and links no backend, no shipper and no telemetry.
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_attempt_vocab.pro && jom && tst_attempt_vocab.exe -o tst_attempt_vocab-out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_attempt_vocab
DESTDIR = $$OUT_PWD

# A private object/moc directory per suite: the suites are built in place in tests/, and a naive
# mtime-based incremental build cannot tell one project's attempt_vocab.obj from another's
# (tst_telemetry.pro's comment on the same trap explains it in full).
OBJECTS_DIR = obj-tst_attempt_vocab
MOC_DIR = obj-tst_attempt_vocab

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_attempt_vocab.cpp \
    ../app/seathub/attempt_vocab.cpp

HEADERS += \
    ../app/seathub/attempt_vocab.h
