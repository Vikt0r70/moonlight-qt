# Facade-level tests (the phase verifier's W2: nothing in this tree linked the facade).
#
# This is the first binary in the tree that compiles `app/seathub/seathub_client.cpp`. The app build
# was previously the only thing that ever saw it, which is how three compile errors reached a tree
# whose every suite was green (03-06 closure, N2).
#
# Two translation units come with it as substitutions, both stitched at their own seam rather than
# faked inside the facade: `moonlight_engine_session.cpp` (needs the whole engine) and
# `pairing_handshake.cpp` (needs a live Sunshine host). See the test file's header for why that is
# the same pattern `engine_session.h` exists for. The engine itself is injected through the public
# `SeatHubClient::session()` accessor, exactly as `tst_engine_seam` injects one.
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_facade_wiring.pro && jom && tst_facade_wiring.exe -o tst_facade_wiring-out.txt,txt

QT += core testlib gui qml network websockets

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_facade_wiring
DESTDIR = $$OUT_PWD

# tst_telemetry.pro also compiles app/seathub/telemetry.cpp, under DIFFERENT DEFINES (it alone
# sets SEATHUB_TEST_ALLOW_LOOPBACK_DSN). See that .pro's own comment on OBJECTS_DIR/MOC_DIR for
# why a shared, flat object directory silently reuses a stale object across the two projects.
OBJECTS_DIR = obj-tst_facade_wiring
MOC_DIR = obj-tst_facade_wiring

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    # shell32: ShellExecuteW, the update feed client's elevated installer launch (D-42).
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2 -lcrypt32 -lshell32
}

# Plan 15 (Task 2): `seathub_client.cpp` now calls `SeatHubTelemetry::*` (`telemetry.h`), so this
# suite links the real `telemetry.cpp` - the same sentry-native include/lib lines as app/app.pro
# and tst_telemetry.pro (06.3.1 D-02). This suite never calls `SeatHubTelemetry::start()` (nothing
# in the facade's own construction or the tests below does), so the SDK is never initialised here:
# every `sentry_*` call `telemetry.cpp` makes is a safe no-op against sentry-native's own lazily-
# initialised global scope (`sentry_scope.c`'s `g_scope`, independent of `sentry_init()`) - real
# linking, no stub.
SEATHUB_SENTRY_DIR = $$(SEATHUB_SENTRY_DIR)
isEmpty(SEATHUB_SENTRY_DIR): SEATHUB_SENTRY_DIR = $$PWD/../build/sentry-native-0.17.1/install
INCLUDEPATH += $$SEATHUB_SENTRY_DIR/include
LIBS += -L$$SEATHUB_SENTRY_DIR/lib -lsentry

SOURCES += \
    tst_facade_wiring.cpp \
    ../app/seathub/seathub_client.cpp \
    ../app/seathub/session_lifecycle.cpp \
    ../app/seathub/engine_session.cpp \
    ../app/seathub/control_plane_client.cpp \
    ../app/seathub/countries.cpp \
    ../app/seathub/region.cpp \
    ../app/seathub/duration_text.cpp \
    ../app/seathub/jordan_time.cpp \
    ../app/seathub/customer_lists.cpp \
    ../app/seathub/token_store.cpp \
    ../app/seathub/session_websocket.cpp \
    ../app/seathub/pairing_controller.cpp \
    ../app/seathub/pairing_seam.cpp \
    ../app/seathub/teardown_controller.cpp \
    ../app/seathub/liveness_timer.cpp \
    ../app/seathub/authorized_through_timer.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/log_shipper.cpp \
    ../app/seathub/telemetry.cpp \
    ../app/seathub/stream_stats.cpp \
    ../app/seathub/engine_termination.cpp \
    ../app/seathub/quality_outbox.cpp \
    ../app/seathub/hud_overlay.cpp \
    ../app/seathub/osd_compositor.cpp \
    ../app/seathub/osd_renderer.cpp \
    ../app/seathub/error_map.cpp \
    ../app/seathub/settings_bridge.cpp \
    ../app/seathub/update_feed_client.cpp \
    ../app/seathub/agent_config.cpp \
    ../app/settings/streamingpreferences.cpp \
    ../app/path.cpp \
    ../app/wm.cpp

# `streamingpreferences.h` is listed so qmake runs moc on it: without its own meta-object the
# upstream class links as four unresolved externals.
HEADERS += \
    ../app/seathub/seathub_client.h \
    ../app/seathub/session_lifecycle.h \
    ../app/seathub/engine_session.h \
    ../app/seathub/control_plane_client.h \
    ../app/seathub/countries.h \
    ../app/seathub/region.h \
    ../app/seathub/web_origin.h \
    ../app/seathub/duration_text.h \
    ../app/seathub/jordan_time.h \
    ../app/seathub/customer_lists.h \
    ../app/seathub/token_store.h \
    ../app/seathub/session_websocket.h \
    ../app/seathub/pairing_controller.h \
    ../app/seathub/pairing_seam.h \
    ../app/seathub/teardown_controller.h \
    ../app/seathub/teardown_guard.h \
    ../app/seathub/liveness_timer.h \
    ../app/seathub/authorized_through_timer.h \
    ../app/seathub/log_tee.h \
    ../app/seathub/log_shipper.h \
    ../app/seathub/telemetry.h \
    ../app/seathub/stream_stats.h \
    ../app/seathub/engine_termination.h \
    ../app/seathub/quality_outbox.h \
    ../app/seathub/hud_overlay.h \
    ../app/seathub/osd_compositor.h \
    ../app/seathub/osd_renderer.h \
    ../app/seathub/error_map.h \
    ../app/seathub/settings_bridge.h \
    ../app/seathub/update_feed_client.h \
    ../app/seathub/agent_config.h \
    ../app/seathub/seathub_version.h \
    ../app/settings/streamingpreferences.h \
    ../app/path.h \
    ../app/utils.h

# The facade exposes the bundled country list, which is read from the binary (Phase 5 plan 06).
RESOURCES += ../app/seathub/countries.qrc

# Plan 14 (D-09/D-15): `SeatHubClient`'s constructor calls `registerOsdFonts()` and
# `handleHostResolved()` registers `OsdCompositor::rasterize` as the engine's text rasteriser, so
# this suite needs the compositor, the pure renderer, and the bundled Open Sans font resource.
RESOURCES += ../app/seathub/fonts.qrc
