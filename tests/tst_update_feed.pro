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

SOURCES += tst_update_feed.cpp \
    ../app/seathub/update_feed_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += ../app/seathub/update_feed_client.h \
    ../app/seathub/error_map.h \
    ../app/seathub/seathub_version.h
