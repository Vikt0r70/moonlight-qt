# Stream record tests (T054, SC23/SC24, ADR-0117): the stream_minute / stream_tail / drop_check
# entries the client writes into the evidence capsule.
#
# Two layers in one binary. The recorder, the journal and the drop check are driven directly with an
# injected clock (so 300 minutes cost no waiting). The last cases drive them THROUGH `SeatHubClient`
# - this suite compiles the facade exactly as tst_facade_wiring.pro does (same source list, same
# engine and handshake substitutions in the test file), because the wiring is part of the contract.
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   tests\run-all-suites.cmd tst_stream_record

QT += core testlib gui qml network websockets

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_stream_record
DESTDIR = $$OUT_PWD

# A private object directory: see tst_facade_wiring.pro for why a shared flat one reuses stale objects.
OBJECTS_DIR = obj-tst_stream_record
MOC_DIR = obj-tst_stream_record

INCLUDEPATH += $$PWD/.. $$PWD/../app

win32 {
    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/include/x64
    LIBS += -L$$PWD/../libs/windows/lib/x64 -lSDL2 -lcrypt32 -ladvapi32
}

SEATHUB_SENTRY_DIR = $$(SEATHUB_SENTRY_DIR)
isEmpty(SEATHUB_SENTRY_DIR): SEATHUB_SENTRY_DIR = $$PWD/../build/sentry-native-0.17.1/install
INCLUDEPATH += $$SEATHUB_SENTRY_DIR/include
LIBS += -L$$SEATHUB_SENTRY_DIR/lib -lsentry

SOURCES += \
    tst_stream_record.cpp \
    ../app/seathub/stream_record.cpp \
    ../app/seathub/drop_check.cpp \
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
    ../app/seathub/sse_client.cpp \
    ../app/seathub/pairing_controller.cpp \
    ../app/seathub/pairing_seam.cpp \
    ../app/seathub/pairing_recovery.cpp \
    ../app/seathub/teardown_controller.cpp \
    ../app/seathub/liveness_timer.cpp \
    ../app/seathub/authorized_through_timer.cpp \
    ../app/seathub/attempt_vocab.cpp \
    ../app/seathub/log_tee.cpp \
    ../app/seathub/log_shipper.cpp \
    ../app/seathub/telemetry.cpp \
    ../app/seathub/install_journal.cpp \
    ../app/seathub/update_retry_state.cpp \
    ../app/seathub/stream_stats.cpp \
    ../app/seathub/stream_quality_sampler.cpp \
    ../app/seathub/engine_termination.cpp \
    ../app/seathub/engine_status.cpp \
    ../app/seathub/quality_outbox.cpp \
    ../app/seathub/end_outbox.cpp \
    ../app/seathub/evidence_journal.cpp \
    ../app/seathub/hud_overlay.cpp \
    ../app/seathub/osd_compositor.cpp \
    ../app/seathub/osd_renderer.cpp \
    ../app/seathub/error_map.cpp \
    ../app/seathub/settings_bridge.cpp \
    ../app/seathub/update_feed_client.cpp \
    ../app/seathub/velopack_updater.cpp \
    ../app/seathub/agent_config.cpp \
    ../app/settings/streamingpreferences.cpp \
    ../app/path.cpp \
    ../app/wm.cpp

HEADERS += \
    ../app/seathub/stream_record.h \
    ../app/seathub/drop_check.h \
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
    ../app/seathub/sse_client.h \
    ../app/seathub/pairing_controller.h \
    ../app/seathub/pairing_seam.h \
    ../app/seathub/teardown_controller.h \
    ../app/seathub/teardown_guard.h \
    ../app/seathub/liveness_timer.h \
    ../app/seathub/authorized_through_timer.h \
    ../app/seathub/log_tee.h \
    ../app/seathub/log_shipper.h \
    ../app/seathub/telemetry.h \
    ../app/seathub/install_journal.h \
    ../app/seathub/update_retry_state.h \
    ../app/seathub/stream_stats.h \
    ../app/seathub/stream_quality_sampler.h \
    ../app/seathub/engine_termination.h \
    ../app/seathub/engine_status.h \
    ../app/seathub/quality_outbox.h \
    ../app/seathub/end_outbox.h \
    ../app/seathub/evidence_journal.h \
    ../app/seathub/hud_overlay.h \
    ../app/seathub/osd_compositor.h \
    ../app/seathub/osd_renderer.h \
    ../app/seathub/stats_catalogue.h \
    ../app/seathub/error_map.h \
    ../app/seathub/settings_bridge.h \
    ../app/seathub/update_feed_client.h \
    ../app/seathub/velopack_updater.h \
    ../app/seathub/agent_config.h \
    ../app/seathub/seathub_version.h \
    ../app/settings/streamingpreferences.h \
    ../app/path.h \
    ../app/utils.h

RESOURCES += ../app/seathub/countries.qrc
RESOURCES += ../app/seathub/fonts.qrc
