#pragma once

// stats_catalogue.h (D-26, D-10, Plan 16 Task 1): the ONE stats catalogue. For each of Moonlight
// 6.1.0's own eleven performance-stats lines (`stringifyVideoStats()`, `ffmpeg.cpp:700-856`,
// read-only), this table holds its SeatHub settings key, Moonlight's own long label
// (`docs/spec/copy.md` § Settings, `settings_bridge.cpp`'s Settings page rows), the OSD's short
// label and unit (`docs/spec/copy.md` § In session, `docs/spec/screens.md` §25/§27), and whether
// it is ticked by default (D-10: RES, FPS and LATENCY on; the other eight off).
//
// Before this file, the eleven key/label pairs lived only in `settings_bridge.cpp`'s own
// `kStatsToggles[]`, and the OSD's short labels/units/order lived only in this plan's own action
// text - two places that could drift apart. This is now the one place both the Settings page
// (`settings_bridge.cpp`) and the OSD compositor (`osd_compositor.cpp`) read the mapping from
// (D-26's "one catalogue").
//
// Header-only, C++11 (`app/app.pro` is `CONFIG += c++11`, unmodified upstream): a `static` array
// plus an `inline` lookup function gives every translation unit its own copy under internal linkage,
// with no ODR risk - the same reasoning `stream_stats.h`'s own header comment gives for avoiding
// `std::optional`.

#include <QString>

struct StatsCatalogueEntry
{
    const char* key;        // the SeatHub QSettings key (`docs/spec/copy.md` § Settings order)
    const char* longLabel;  // Moonlight's own line text, verbatim (no trailing colon)
    const char* shortLabel; // the OSD's short label (uppercase, `docs/spec/copy.md` § In session)
    const char* unit;       // the OSD's unit suffix, or "" for none (resolution has no unit)
    bool defaultOn;         // D-10: true only for RES, FPS and LATENCY
};

// Moonlight's own eleven performance-stats lines, in `stringifyVideoStats()`'s own output order
// (`ffmpeg.cpp:700-856`) - the same order `docs/spec/copy.md` § Settings and § In session list
// them in.
static const StatsCatalogueEntry kStatsCatalogue[] = {
    { "statsVideoStream", "Video stream", "RES", "", true },
    { "statsIncomingFrameRate", "Incoming frame rate from network", "RECV", "fps", false },
    { "statsDecodingFrameRate", "Decoding frame rate", "DEC", "fps", false },
    { "statsRenderingFrameRate", "Rendering frame rate", "FPS", "fps", true },
    { "statsHostProcessingLatency", "Host processing latency min/max/average", "HOST", "ms",
      false },
    { "statsNetworkDroppedFrames", "Frames dropped by your network connection", "LOSS", "%",
      false },
    { "statsJitterDroppedFrames", "Frames dropped due to network jitter", "JITTER", "%", false },
    { "statsNetworkLatency", "Average network latency", "LATENCY", "ms", true },
    { "statsDecodingTime", "Average decoding time", "DECODE", "ms", false },
    { "statsFrameQueueDelay", "Average frame queue delay", "QUEUE", "ms", false },
    { "statsRenderingTime", "Average rendering time (including monitor V-sync latency)", "RENDER",
      "ms", false },
};

static const int kStatsCatalogueCount =
    static_cast<int>(sizeof(kStatsCatalogue) / sizeof(kStatsCatalogue[0]));

/// Looks up a catalogue row by its SeatHub settings key (`statsVideoStream`, ...). Returns
/// `nullptr` for an unknown key.
inline const StatsCatalogueEntry* findStatsCatalogueEntry(const QString& key)
{
    for (const StatsCatalogueEntry& entry : kStatsCatalogue) {
        if (key == QLatin1String(entry.key)) {
            return &entry;
        }
    }
    return nullptr;
}
