#include "app/scan_age.hpp"

namespace cappedweather {
namespace {

// "HH:mm:ssZ" for a scan time the caller can vouch for, an em dash otherwise.
// toUTC() is explicit: the block is honest UTC, never the local clock.
QString scanTimeText(const QDateTime& scanTimeUtc)
{
    if (!scanTimeUtc.isValid()) {
        return QStringLiteral("—");
    }
    return scanTimeUtc.toUTC().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("Z");
}

QString rayCountText(const int rayCount)
{
    return rayCount >= 0 ? QString::number(rayCount) : QStringLiteral("—");
}

} // namespace

ScanAgeText scanAgeText(const QDateTime& scanTimeUtc, const int rayCount,
                        const qint64 ageSeconds, const int volumeCoveragePattern)
{
    // A clock behind the scan time must not print "-1 min old"; the clamp also
    // feeds the stale predicate so the printed age and the colour agree.
    const auto age = std::max<qint64>(0, ageSeconds);
    const auto timeText = scanTimeText(scanTimeUtc);
    const auto minutesText = QString::number(age / 60);
    const auto raysText = rayCountText(rayCount);
    if (volumeCoveragePattern > 0) {
        return {QStringLiteral("SCAN %1 · VCP %2 · %3 min old · %4 rays")
                    .arg(timeText, QString::number(volumeCoveragePattern), minutesText, raysText),
                scanAgeIsStale(age)};
    }
    return {QStringLiteral("SCAN %1 · %2 min old · %3 rays").arg(timeText, minutesText, raysText),
            scanAgeIsStale(age)};
}

QString scanAgeEmptyStateText()
{
    // Says what the block is waiting for. It must not read as a scan that is
    // loading, and it must not read as one that is live.
    return QStringLiteral("Waiting for latest scan");
}

QString scanAgeEmptyStateTooltip()
{
    return QStringLiteral("Pick a station to load the latest scan");
}

} // namespace cappedweather
