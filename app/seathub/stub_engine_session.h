#pragma once

// The Plan 03-02 tracer, as an `EngineSession`.
//
// It began life as a fallback inside `SessionLifecycle`: when no engine session was attached it
// ran a timed sequence that emitted the same seven lifecycle signals and showed a real SDL window,
// which is how the D-01/D-03 visibility sequence was proven without a Sunshine host. That made it
// the *default* production path - and since nothing ever called `attachSession()`, it was the only
// path any customer would have got. Plan 03-06's gap closure moved it behind the seam and made it
// opt-in: `SessionLifecycle` now fails closed when nothing is attached, and this class exists to be
// constructed deliberately.
//
// Two things keep it honest as a fake:
//
//   * Its window carries the same title the engine's does (`'<name> - SeatHub'`, ADR-0046), so
//     "no Moonlight on screen" is checkable from a probe rather than only from the source.
//   * Its window is real SDL, created and destroyed in the same order the engine uses, which is
//     what makes the visibility sequence observable rather than asserted.
//
// It is NOT a stream: no host, no decoder, no video. `publishOverlaySurface` has nothing to
// composite into and refuses every surface, which is the same answer the production session gives
// when no stream is running.

#include <QString>

#include "engine_session.h"

class QTimer;
class QWindow;

class StubEngineSession : public EngineSession
{
    Q_OBJECT

public:
    explicit StubEngineSession(QObject* parent = nullptr);
    ~StubEngineSession() override;

    /// Starts the timed sequence and returns immediately. The production session blocks for the
    /// whole stream; this one drives itself from the event loop, which is what lets the sequence be
    /// watched from a running Qt view.
    void run(QWindow* window) override;

    /// Jumps straight to the teardown leg, so D-02 is exercisable without a host.
    void interrupt() override;

    /// Refuses every surface: there is no swapchain here. A non-null surface is released, because
    /// the publisher's ownership contract says it transfers in every case.
    bool publishOverlaySurface(SDL_Surface* surface) override;

    /// The name segment of the stream window's title. Defaults to `SeatHub tracer`.
    void setRigName(const QString& name);

private slots:
    void runStage(int step);

private:
    void createStreamWindow();
    void destroyStreamWindow();
    void finish();
    void stop();

    /// True when the Qt window the sequence is driving is on screen. Used by this class's own
    /// diagnostics, which is how the D-01/D-03 sequence is provable from a run's log rather than
    /// only from its source.
    bool qtWindowVisible() const;

    QWindow* m_qtWindow = nullptr;
    QString m_rigName;

    QTimer* m_timer = nullptr;
    int m_step = 0;
    bool m_ownsVideoSubsystem = false;
    void* m_window = nullptr;   // SDL_Window*, kept void* so SDL.h stays out of this header
};
