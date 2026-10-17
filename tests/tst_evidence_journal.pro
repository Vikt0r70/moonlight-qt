# Evidence journal tests (ADR-0105): per-Play closed-value capsule files, caps, reduction, counters, answers, replay and privacy.
#
# Qt Core only (`QT += core testlib`): no SDL, no Windows API, no network, no GUI module. It builds
# on Linux through tests/linux/run-core-suites.sh as well as on Windows.
#
# Build recipe (Windows; nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_evidence_journal.pro && jom && tst_evidence_journal.exe -o tst_evidence_journal-out.txt,txt
# Linux: bash tests/linux/run-core-suites.sh --provision tst_evidence_journal

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_evidence_journal
DESTDIR = $$OUT_PWD

# A private object/moc directory per suite (see tst_attempt_vocab.pro for the trap this avoids).
OBJECTS_DIR = obj-tst_evidence_journal
MOC_DIR = obj-tst_evidence_journal

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_evidence_journal.cpp \
    ../app/seathub/evidence_journal.cpp \
    ../app/seathub/attempt_vocab.cpp

HEADERS += \
    ../app/seathub/evidence_journal.h \
    ../app/seathub/attempt_vocab.h \
    ../app/seathub/port_test_result.h
