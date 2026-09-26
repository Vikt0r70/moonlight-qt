#pragma once

// The production `EngineSession`: upstream Moonlight's own `Session`, built from the host the
// pairing handshake resolved and one of the applications that host serves.
//
// This is the only header in `app/seathub/` that reaches into `app/backend/`, and
// `moonlight_engine_session.cpp` is the only translation unit that constructs, attaches to or
// drives the engine itself (`Session`). Since Plan 14 this header also includes
// `streaming/video/overlaymanager.h` for one typedef (`setTextRasterizer()`'s own comment says
// why) - a type-only reach, the same as `osd_compositor.h` already does. Everything else about
// the fork boundary is unchanged: no engine file is edited to construct, attach or drive the
// session.

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QVector>

#include "engine_session.h"

// D-09/D-13 (ADR-0045 amended 2026-09-26, Plan 14): `setTextRasterizer()` below needs
// `Overlay::OverlayManager::TextRasterizer` in its own signature (the caller passes
// `&OsdCompositor::rasterize` with no cast), so this header now also reaches into
// `app/streaming/` for that one typedef - `osd_compositor.h` (Plan 10) already does the same for
// the same reason. `moonlight_engine_session.cpp` remains the only translation unit that
// constructs, attaches to or drives the engine itself; this include is type-only.
//
// `overlaymanager.h` pulls in unprotected `<SDL.h>`, which `#define`s `main` to `SDL_main` unless
// something defines `SDL_MAIN_HANDLED` first (`app/main.cpp`'s own reasoning; `hud_overlay.h`
// guards itself the same way for the same reason). Since `seathub_client.h` includes this header,
// and `tst_facade_wiring.cpp` includes `seathub_client.h` without its own SDL guard, an
// unprotected include here silently renamed `QTEST_MAIN`'s own `main()` to `SDL_main` and broke
// the link (`LNK2019: unresolved external symbol main`) - fixed by guarding here, exactly as
// `hud_overlay.h` already does, so every includer is protected transitively.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "streaming/video/overlaymanager.h"

class NvApp;
class NvComputer;
class NvHTTP;
class Session;

/// The host the handshake paired with, plus the application list it read from the same host.
///
/// Held by `shared_ptr` so the handshake (which resolves it, on a pool thread) can hand it to the
/// facade (which builds the session, on the Qt main thread) through a signal, with no raw pointer
/// crossing in either direction.
class MoonlightPairedHost : public PairedHost
{
public:
    /// Builds the record from the `serverinfo` response the handshake already fetched. Upstream's
    /// `NvComputer` constructor is what fills in the address, the HTTPS port, the certificate and
    /// the host's own reported version.
    MoonlightPairedHost(NvHTTP& http, const QString& serverInfo);
    ~MoonlightPairedHost() override;

    /// Pins the certificate `NvPairingManager::pair()` returned and marks the record streamable.
    /// Until this has been called the record is the pre-pairing view of the host and must not be
    /// streamed from.
    void pinCertificate(const QByteArray& pem);
    bool isPaired() const { return m_paired; }

    /// Reads the host's own application list (`GET /applist`, upstream's `NvHTTP::getAppList()`).
    /// Called from the handshake's pool thread, so no frame of the Qt main thread's work waits on
    /// a network round trip. A failure is recorded, never reported as an empty host: the caller
    /// fails closed on `launchApp()` instead.
    void fetchAppList();
    bool hasAppList() const { return m_appListRead; }

    /// The application to stream, resolved from the host's own list.
    ///
    /// False when the host never answered, or when its list does not determine a *single*
    /// application. The frozen control-plane contract (`docs/spec/openapi.yaml` 1.6.0,
    /// `SessionAuthorization` and `Session`) carries no application id, so a rig that serves
    /// several is a rig this client cannot choose for - and choosing one, or taking the first,
    /// would be inventing the value the contract omits. `ADR-0048` records the gap and what closes
    /// it. Selecting nothing is the honest answer.
    bool launchApp(NvApp* out) const;

    NvComputer* computer() const { return m_computer; }

private:
    NvComputer* m_computer;
    bool m_paired = false;
    bool m_appListRead = false;
};

/// Upstream's `Session`, behind the engine seam.
class MoonlightEngineSession : public EngineSession
{
    Q_OBJECT

public:
    /// Builds the session for a host the handshake resolved, or returns null when there is
    /// nothing to stream from: a host that is not a `MoonlightPairedHost` (the only producer is
    /// `runUpstreamPairingHandshake`), or one whose application list does not name a single
    /// application. The caller fails closed on null; it must not substitute a stub.
    static MoonlightEngineSession* create(const PairedHostPtr& host, QObject* parent = nullptr);

    /// `host` is held for the session's whole life: upstream's `Session` keeps a raw
    /// `NvComputer*` and reads it - address, ports, pinned certificate - throughout. `app` is
    /// copied in, exactly as upstream's own `AppModel`/`ComputerModel` pass it.
    MoonlightEngineSession(PairedHostPtr host, const NvApp& app, QObject* parent = nullptr);
    ~MoonlightEngineSession() override;

    void run(QWindow* window) override;
    void interrupt() override;
    bool publishOverlaySurface(SDL_Surface* surface) override;

    /// CUST-17/D-23: sets which of the engine's own OverlayDebug lines may be drawn, from the
    /// customer's current settings choice (`SettingsBridge::enabledStatsLabels()`, D-26). A thin
    /// forward to the attached engine's own compositor (`OverlayManager::setDebugLineFilter()`,
    /// the D-28 exception) - this class holds no filtering logic of its own. Safe to call before
    /// `run()` starts the stream; the compositor reads the filter fresh on its own next
    /// rasterise, so calling this before the engine ever writes a stats line is enough.
    void setDebugLineFilter(const QStringList& enabledLabels);

    /// D-09/D-13 (ADR-0045 amended 2026-09-26): a thin forward to the attached engine's own
    /// compositor (`OverlayManager::setTextRasterizer()`, the D-28 exception) - this class holds
    /// no rasterising logic of its own, the same idiom as `setDebugLineFilter()` above. Safe to
    /// call before `run()` starts the stream; the manager swaps the rasteriser lock-free, the
    /// same way it swaps the debug-line filter.
    void setTextRasterizer(Overlay::OverlayManager::TextRasterizer rasterizer, void* context);

private:
    PairedHostPtr m_host;
    Session* m_engine = nullptr;
};
