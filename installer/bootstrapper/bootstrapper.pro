# The RentaPC setup window (ADR-0107): plain Win32, no Qt runtime, built with the toolchain that
# builds the rest of the fork. scripts/pack-seathub.ps1 builds it after `vpk pack`:
#
#   qmake installer\bootstrapper\bootstrapper.pro "STAGE_DIR=<folder holding inner-setup.bin and
#         bootstrapper-version.rci>"   then   nmake release
#
# STAGE_DIR is where the packer puts the unchanged Velopack Setup (inner-setup.bin) and the version
# include; the resource compiler finds both there.

TEMPLATE = app
TARGET = RentaPC-Setup
CONFIG -= qt
CONFIG += windows release c++17
CONFIG += static_runtime
CONFIG -= console
QT =

SOURCES += main.cpp
INCLUDEPATH += $$PWD/../../app/seathub
RC_FILE = $$PWD/bootstrapper.rc
RC_INCLUDEPATH += $$STAGE_DIR

DEFINES += UNICODE _UNICODE
QMAKE_CXXFLAGS += /utf-8
QMAKE_LFLAGS += /SUBSYSTEM:WINDOWS
QMAKE_LFLAGS += "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
