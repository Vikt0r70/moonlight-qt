# SeatHub's one log tee, the end-of-stream video-stats parser, and the engine termination matcher
# (Plan 15, D-17/A-51, ADR-0044).
#
# Links `log_tee.cpp`, `stream_stats.cpp` and `engine_termination.cpp` directly, plus
# `liveness_timer.cpp`/`control_plane_client.cpp`/`error_map.cpp` (the same set `tst_liveness.pro`
# links standalone) so the "the tee's termination sink reaches `noteTermination`" test can drive a
# real `LivenessTimer` and read its `hasEngineError()`/`engineError()` accessors - the observable
# state `noteTermination()` sets whether or not a control plane is attached (`reportNow()` no-ops
# on the POST with none set, but the state fields it reads are already written by then).
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_stream_stats.pro && jom && tst_stream_stats.exe -o tst_stream_stats-out.txt,txt

QT += core testlib network

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_stream_stats
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2
}

SOURCES += \
    tst_stream_stats.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/stream_stats.cpp \
    ../app/seathub/engine_termination.cpp \
    ../app/seathub/liveness_timer.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/error_map.cpp

HEADERS += \
    ../app/seathub/log_tee.h \
    ../app/seathub/stream_stats.h \
    ../app/seathub/engine_termination.h \
    ../app/seathub/liveness_timer.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/error_map.h
