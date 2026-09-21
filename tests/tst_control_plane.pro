# Control-plane HTTP client tests (Plan 03-03 Task 1).
#
# Links the client and the failure model it reports through (`error_map.cpp`, D-51). No server
# is contacted: the responses are injected through a fake QNetworkAccessManager.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_control_plane.pro && jom release && tst_control_plane.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_control_plane
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_control_plane.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/countries.cpp \
    ../app/seathub/region.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/countries.h \
    ../app/seathub/region.h \
    ../app/seathub/web_origin.h \
    ../app/seathub/error_map.h

# The bundled country list is read from the binary, so the binary carries it (Phase 5 plan 06).
RESOURCES += ../app/seathub/countries.qrc
