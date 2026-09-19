# Multi-threaded path tests (CR-01, CR-02, HR-01).
#
# The only suite in this tree that creates a real `QThread` and moves real objects onto it, because
# that is the coverage whose absence let three defects through: a double free in
# `ControlPlaneClient::stopOwnedThread()`, two billing timers started from the wrong thread, and a
# teardown controller whose verify timer was refused its start.
#
# Links the real `TokenStore` (`crypt32`) on purpose: the teardown test asserts the DPAPI blob it
# stored is gone, which is what "leaves nothing behind" (STREAM-10) means in practice.
#
# Build recipe:
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_threading.pro && jom release && tst_threading.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_threading
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

LIBS += -lcrypt32

SOURCES += \
    tst_threading.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/liveness_timer.cpp \
    ../app/seathub/authorized_through_timer.cpp \
    ../app/seathub/teardown_controller.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/liveness_timer.h \
    ../app/seathub/authorized_through_timer.h \
    ../app/seathub/teardown_controller.h \
    ../app/seathub/token_store.h \
    ../app/seathub/error_map.h
