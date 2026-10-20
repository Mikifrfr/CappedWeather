#pragma once

// SCAN age label policy, extracted from src/app/main_window.cpp.
//
// The block under the playback transport reports the honest UTC scan time of
// the frame on screen, the volume's coverage pattern when the decoder can vouch
// for one, the whole-minute age and the ray count:
//
//   SCAN 14:22:10Z · VCP 212 · 3 min old · 1080 rays
//
// It replaced the old LIVE dot and never claims liveness: it reports age, and
// the amber styling is the only stale signal. This module owns the wording, the
// 0-minute clamp, the em dash for values the caller cannot vouch for, the VCP
// token rule and the stale predicate. It deliberately does not own the clock
// read, the widget writes or the colour: those stay in
// MainWindow::refreshScanAgeLabel, the only place that knows a frame is on
// screen.

#include <QDateTime>
#include <QString>

#include <algorithm>

namespace cappedweather {

// Whole-minute age at which the SCAN block turns amber. Matches the radar-health
// Online bound (data older than 5 min is stale, not live).
inline constexpr qint64 kScanStaleSeconds = 300;

// Wording plus the one derived fact the caller needs to colour it. Keeping
// `stale` beside the label is what stops the widget from re-deriving the
// threshold from a different age value than the one that was printed.
struct ScanAgeText {
    QString label;
    bool stale = false;
};

// Defined here rather than in the .cpp so it stays usable in constant
// expressions (and so the threshold has exactly one home). A negative age -- a
// clock behind the scan time -- is never stale: the age printed is clamped to
// zero, and comparing the unclamped value would only reproduce that clamp.
constexpr bool scanAgeIsStale(const qint64 ageSeconds) noexcept
{
    return ageSeconds >= kScanStaleSeconds;
}

// `scanTimeUtc` invalid renders an em dash (a spilled frame whose header could
// not be read); `rayCount` < 0 likewise (the volume's disk copy is unreadable).
// `volumeCoveragePattern` <= 0 means no pattern the decoder can vouch for --
// either none was parsed or the volume's radials disagree -- and prints no VCP
// token at all, because a labelled-but-empty "VCP" reads like a value that
// failed to load.
ScanAgeText scanAgeText(const QDateTime& scanTimeUtc, int rayCount, qint64 ageSeconds,
                        int volumeCoveragePattern = 0);

// The no-frames state of the same block: shown before any volume is on screen,
// and again after the session is cleared. Public so the wording is testable and
// so no caller can invent a second version of it.
QString scanAgeEmptyStateText();
QString scanAgeEmptyStateTooltip();

} // namespace cappedweather
