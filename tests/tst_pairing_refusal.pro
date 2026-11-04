# Refused-start tests (M002 T012, SC5): the 412 `session.no_verified_path` class, the sentence per
# reason, no dial, the `session_refused` end stage, and the `attempt_record side=client` line plus
# the `attempt` evidence entry for every attempt.
#
# Links the pairing controller and seam (the seam's file holds the record formatter), the HTTP
# client the controller polls through, the failure model and the evidence journal. No server and no
# Sunshine host are involved: the control-plane answers are injected through a fake
# QNetworkAccessManager and the engine seam is a recording fake.
#
# Build recipe (nothing is on PATH; see `seathub-ops/pins.yaml`):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_pairing_refusal.pro && jom && tst_pairing_refusal.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_pairing_refusal
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

SOURCES += \
    tst_pairing_refusal.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/log_shipper.cpp \
    ../app/seathub/pairing_controller.cpp \
    ../app/seathub/pairing_seam.cpp \
    ../app/seathub/pairing_recovery.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp \
    ../app/seathub/evidence_journal.cpp \
    ../app/seathub/attempt_vocab.cpp

HEADERS += \
    ../app/seathub/pairing_controller.h \
    ../app/seathub/pairing_seam.h \
    ../app/seathub/pairing_recovery.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h \
    ../app/seathub/evidence_journal.h \
    ../app/seathub/attempt_vocab.h
