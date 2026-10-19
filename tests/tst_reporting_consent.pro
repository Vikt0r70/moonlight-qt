# Install-time reporting choice (ADR-0107, T021): the store the RentaPC setup window writes, the one
# switch every covered sender asks, the silent-install and notice-version rules, and "Sentry is never
# initialised without a recorded send". Links the real sentry-native like tst_telemetry.pro because the
# last case drives SeatHubTelemetry::startWith() itself (it must refuse to init, so no handler runs).
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
#   cd tests && qmake tst_reporting_consent.pro && jom && tst_reporting_consent.exe -o tst_reporting_consent-out.txt,txt

QT += core gui qml network testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_reporting_consent
DESTDIR = $$OUT_PWD

# telemetry.cpp is also compiled by tst_telemetry.pro and tst_facade_wiring.pro under different
# defines; a private object directory per suite keeps a naive incremental build from mixing them.
OBJECTS_DIR = obj-tst_reporting_consent
MOC_DIR = obj-tst_reporting_consent

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

SEATHUB_SENTRY_DIR = $$(SEATHUB_SENTRY_DIR)
isEmpty(SEATHUB_SENTRY_DIR): SEATHUB_SENTRY_DIR = $$PWD/../build/sentry-native-0.17.1/install
INCLUDEPATH += $$SEATHUB_SENTRY_DIR/include
LIBS += -L$$SEATHUB_SENTRY_DIR/lib -lsentry

# acceptDsn() accepts http to 127.0.0.1 in THIS test binary only (same widening tst_telemetry.pro uses).
DEFINES += SEATHUB_TEST_ALLOW_LOOPBACK_DSN

win32: LIBS += -lcrypt32 -ladvapi32 -luser32 -lgdi32 -lcomctl32 -lshell32

SOURCES += \
    tst_reporting_consent.cpp \
    ../app/seathub/reporting_consent.cpp \
    ../app/seathub/telemetry.cpp \
    ../app/seathub/install_journal.cpp \
    ../app/seathub/update_retry_state.cpp \
    ../app/seathub/stream_stats.cpp \
    ../app/seathub/stream_quality_sampler.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/log_shipper.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/error_map.cpp \
    ../app/path.cpp

HEADERS += \
    ../app/seathub/reporting_consent.h \
    ../app/seathub/reporting_consent_store.h \
    ../app/seathub/telemetry.h \
    ../app/seathub/install_journal.h \
    ../app/seathub/update_retry_state.h \
    ../app/seathub/stream_stats.h \
    ../app/seathub/stream_quality_sampler.h \
    ../app/seathub/log_tee.h \
    ../app/seathub/log_shipper.h \
    ../app/seathub/token_store.h \
    ../app/seathub/error_map.h \
    ../app/path.h
