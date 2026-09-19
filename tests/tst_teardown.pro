# Teardown tests (Plan 03-03 Task 2, STREAM-10).
#
# Links the real `TokenStore` on purpose: "leaves nothing behind" is only a real claim if the
# credential it clears was a real DPAPI blob on a real disk, so this test stores one and checks
# the file afterwards. `crypt32` is the same library the application links.
#
# Build recipe:
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_teardown.pro && jom && tst_teardown.exe

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_teardown
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

LIBS += -lcrypt32

SOURCES += \
    tst_teardown.cpp \
    ../app/seathub/teardown_controller.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/teardown_controller.h \
    ../app/seathub/teardown_guard.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/token_store.h \
    ../app/seathub/error_map.h
