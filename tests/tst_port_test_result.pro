# Port-test result tests (ADR-0104): the LiTestClientConnectivity mask as a closed class, the wire port_test object and the firewall-sentence condition.
#
# Qt Core only (`QT += core testlib`): no SDL, no Windows API, no network, no GUI module. It builds
# on Linux through tests/linux/run-core-suites.sh as well as on Windows.
#
# Build recipe (Windows; nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_port_test_result.pro && jom && tst_port_test_result.exe -o tst_port_test_result-out.txt,txt
# Linux: bash tests/linux/run-core-suites.sh --provision tst_port_test_result

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_port_test_result
DESTDIR = $$OUT_PWD

# A private object/moc directory per suite (see tst_attempt_vocab.pro for the trap this avoids).
OBJECTS_DIR = obj-tst_port_test_result
MOC_DIR = obj-tst_port_test_result

INCLUDEPATH += $$PWD/.. $$PWD/../app

SOURCES += \
    tst_port_test_result.cpp

HEADERS += \
    ../app/seathub/port_test_result.h
