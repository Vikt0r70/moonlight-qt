# Silent-pairing tests (Plan 03-03 Task 2, STREAM-03).
#
# Links the pairing controller, the HTTP client it polls through, and the failure model. No
# server and no Sunshine host are involved: the control-plane responses are injected through a
# fake QNetworkAccessManager and the engine is a recording fake seam.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_pairing.pro && jom && tst_pairing.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_pairing
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_pairing.cpp \
    ../app/seathub/pairing_controller.cpp \
    ../app/seathub/pairing_seam.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/pairing_controller.h \
    ../app/seathub/pairing_seam.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h
