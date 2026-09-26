# sentry-native crash reporting tests (06.3.1 D-01, D-02, D-05).
#
# Exercises the real sentry-native + crashpad stack end to end: a child process crashes for real
# (a null write), and the parent's own fake Sentry endpoint (a QTcpServer) receives the minidump
# POST crashpad's out-of-process handler sends. Nothing at the sentry_init level is mocked - the
# two things substituted are the ingest endpoint (a loopback listener instead of sentry.io) and the
# handler folder (a temp copy holding crashpad_handler.exe alone, so these tests never register a
# WER helper-module value on the machine that runs them - see SEATHUB_SENTRY_BIN_DIR below).
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
#   cd tests && qmake tst_telemetry.pro && jom && tst_telemetry.exe -o tst_telemetry-out.txt,txt
#
# At runtime, sentry.dll (linked, not statically), crashpad_handler.exe and crashpad_wer.dll's own
# install `bin` folder must be on PATH - tests/run-all-suites.cmd's PATH line carries it.

QT += core network testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_telemetry
DESTDIR = $$OUT_PWD

# tst_facade_wiring.pro also compiles app/seathub/telemetry.cpp, under DIFFERENT DEFINES (this
# project alone sets SEATHUB_TEST_ALLOW_LOOPBACK_DSN, below). Both projects' object/moc files
# otherwise land flat in tests/ (default OBJECTS_DIR/MOC_DIR), where a naive mtime-based
# incremental build cannot tell one project's telemetry.obj from the other's and silently reuses
# whichever is newer - losing the loopback override with no build error. A private directory per
# suite keeps the two from ever colliding.
OBJECTS_DIR = obj-tst_telemetry
MOC_DIR = obj-tst_telemetry

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

# The same sentry-native include/lib lines as app/app.pro (06.3.1 D-02).
SEATHUB_SENTRY_DIR = $$(SEATHUB_SENTRY_DIR)
isEmpty(SEATHUB_SENTRY_DIR): SEATHUB_SENTRY_DIR = $$PWD/../build/sentry-native-0.17.1/install
INCLUDEPATH += $$SEATHUB_SENTRY_DIR/include
LIBS += -L$$SEATHUB_SENTRY_DIR/lib -lsentry

# Where the real crashpad_handler.exe/crashpad_wer.dll/sentry.dll live, so a test can build a temp
# folder holding the handler alone (behaviour 4: no WER module ever registers on this machine).
DEFINES += SEATHUB_SENTRY_BIN_DIR=\\\"$$SEATHUB_SENTRY_DIR/bin\\\"
DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

# Plan 15: `acceptDsn()`'s real rule is https + `.ingest.de.sentry.io` only - this widens it, in
# THIS TEST BINARY ONLY, to also accept http to 127.0.0.1, the loopback address every crash child
# in this suite uses as its fake Sentry endpoint (mirrors the rig agent's own `#[cfg(test)]`
# widening of `accept_dsn`, 06.3.1-07-PLAN.md). Never defined for app.pro or any other suite, so a
# release build can never accept an unencrypted loopback DSN (T-06.3.1-41).
DEFINES += SEATHUB_TEST_ALLOW_LOOPBACK_DSN

win32: LIBS += -lcrypt32

SOURCES += \
    tst_telemetry.cpp \
    ../app/seathub/telemetry.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/error_map.cpp \
    ../app/path.cpp

HEADERS += \
    ../app/seathub/telemetry.h \
    ../app/seathub/log_tee.h \
    ../app/seathub/token_store.h \
    ../app/seathub/error_map.h \
    ../app/path.h
