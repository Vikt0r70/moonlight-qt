# Release-feed and forced-update tests (Plan 03-04 Task 2).
#
# Links the feed client and the failure model it reports through (`error_map.cpp`, D-51). No
# framework besides Qt: the checksum check is verified against NIST's published vectors over a
# local temporary file.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_update_feed.pro && jom release && tst_update_feed.exe

QT += core testlib qml quick quickcontrols2 network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_update_feed
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

# Velopack (ADR-0086): the suite compiles the updater seam WITHOUT SEATHUB_VELOPACK, so the real
# Velopack C library is neither included nor linked; every test drives a fake UpdateApplier.
# advapi32: RegDeleteTreeW, the uninstall hook's HKCU key removal.
win32: LIBS += -ladvapi32

SOURCES += tst_update_feed.cpp \
    ../app/seathub/update_feed_client.cpp \
    ../app/seathub/velopack_updater.cpp \
    ../app/seathub/update_retry_state.cpp \
    ../app/seathub/install_journal.cpp \
    ../app/seathub/error_map.cpp

HEADERS += ../app/seathub/update_feed_client.h \
    ../app/seathub/velopack_updater.h \
    ../app/seathub/update_retry_state.h \
    ../app/seathub/install_journal.h \
    ../app/seathub/error_map.h \
    ../app/seathub/seathub_version.h
