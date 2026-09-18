import QtQuick

// SeatHub's view of the engine lifecycle. Replaces upstream `StreamSegue.qml`.
//
// Two things live here and nothing else does:
//
//   1. The D-01 visibility sequence. The Qt window is visible before streaming starts, is
//      hidden the moment the engine reports `connectionStarted` (which the engine emits
//      *before* it creates its SDL window), and is restored only when SDL destruction is
//      proven. One visible window at a time, and no launcher frame behind the stream.
//   2. The D-03 restore rule. `readyForDeletion` is the only signal that restores the Qt
//      window. `quitStarting` fires before the engine's deferred cleanup and before SDL
//      destroys its window, so restoring there would put two windows on screen at once
//      (Pitfall 1).
//
// The seven engine signals are observed by `SessionLifecycle` (C++), which re-emits them
// under the same names. Wiring them here as well would run every handler twice, so this
// file subscribes to the lifecycle rather than to the engine Session directly.
Item {
    id: segue

    // The Qt window the sequence hides and restores (D-01). Named `window` to keep the
    // visibility lines identical to upstream's StreamSegue.qml.
    property var window

    // The SeatHub lifecycle that owns the engine Session - `client.session`.
    property var lifecycle

    width: 0
    height: 0

    // --- The seven lifecycle signals, each with a SeatHub-owned handler. ---
    Connections {
        target: segue.lifecycle

        function onStageStarting(stage) {
            segue.stageStarting(stage)
        }

        function onStageFailed(stage, errorCode, failingPorts) {
            segue.stageFailed(stage, errorCode, failingPorts)
        }

        function onConnectionStarted() {
            segue.connectionStarted()
        }

        function onDisplayLaunchError(text) {
            segue.displayLaunchError(text)
        }

        function onQuitStarting() {
            segue.quitStarting()
        }

        function onSessionFinished(portTestResult) {
            segue.sessionFinished(portTestResult)
        }

        function onReadyForDeletion() {
            segue.sessionReadyForDeletion()
        }
    }

    function stageStarting(stage) {
        // Nothing to do at the window level. The client turns the stage into a
        // docs/spec/copy.md §Play flow line for the connecting view.
        console.debug("SeatHub: stream stage started -", stage)
    }

    function stageFailed(stage, errorCode, failingPorts) {
        // The engine's stage name, error code and failing ports are diagnostics. They are
        // never rendered; the client maps them to SeatHub copy plus an ADR-0008 reference.
        console.debug("SeatHub: stream stage failed -", stage, errorCode, failingPorts)
    }

    function connectionStarted() {
        // D-01: hide the Qt window now. The engine creates its SDL window after this
        // signal, so the two are never visible together and nothing of the launcher is
        // left behind the stream.
        window.visible = false
    }

    function displayLaunchError(text) {
        // Never rendered verbatim (T-03-05). Log only; the client maps it to SeatHub copy.
        console.debug("SeatHub: engine launch error intercepted (not shown to the customer)")
    }

    function quitStarting() {
        // Deliberately does NOT restore the Qt window. The engine fires this before its
        // deferred cleanup runs and before SDL destroys its window, so restoring here
        // would show two windows at once (Pitfall 1). See sessionReadyForDeletion().
    }

    function sessionFinished(portTestResult) {
        // The engine has stopped; SDL destruction is still pending. Still no restore.
        console.debug("SeatHub: session finished, port test result", portTestResult)
    }

    function sessionReadyForDeletion() {
        // D-03: SDL destruction is proven by the time this arrives, so the Qt window may
        // come back. This is the only place the window is restored.
        window.visible = true
    }
}
