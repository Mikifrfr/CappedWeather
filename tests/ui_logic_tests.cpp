// Hermetic checks for cappedweather_ui_logic: the Qt-Core+Gui-only library that
// hosts QString/QDateTime/QFont policy extracted from src/app/main_window.cpp.
//
// Deliberately not part of tests/ui_smoke_test.cpp (frozen for the extraction
// series), and deliberately built on QGuiApplication: a module added to the
// library that needs QApplication or QWidget stops compiling here instead of
// quietly turning the extraction target into a second UI suite.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/Release/cappedweather_ui_logic_tests

#include "app/scan_age.hpp"
#include "app/ui_logic/anchor.hpp"

#include <QGuiApplication>
#include <QTimeZone>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) {
        std::printf("PASS %s\n", what);
        return;
    }
    ++failures;
    std::printf("FAIL %s\n", what);
}

// Exact-string checks carry both sides: the label is the deliverable here, so a
// near miss ("3 min" vs "03 min", a hyphen instead of the em dash) has to be
// readable from the log without rebuilding.
void checkString(const QString& got, const QString& expected, const char* what)
{
    if (got == expected) {
        std::printf("PASS %s\n", what);
        return;
    }
    ++failures;
    std::printf("FAIL %s\n      expected \"%s\"\n      got      \"%s\"\n", what,
                expected.toUtf8().constData(), got.toUtf8().constData());
}

// S0 acceptance: the library links and its seed symbol resolves. An empty or
// missing anchor means the static library was not linked into this executable
// (or the symbol was renamed), i.e. the target is not yet usable as the home
// for extracted policy.
void testUiLogicTargetLinks()
{
    const char* anchor = cappedweatherUiLogicAnchor();
    check(anchor != nullptr, "anchor pointer is non-null");
    if (anchor == nullptr) {
        return;
    }
    check(std::strlen(anchor) > 0, "anchor string is non-empty");
    check(std::strncmp(anchor, "cappedweather-ui-logic/",
                       sizeof("cappedweather-ui-logic/") - 1) == 0,
          "anchor names the library");
    std::printf("      anchor=%s\n", anchor);
}

// --- Step 1 (A1): SCAN age block -------------------------------------------------
//
// The exact strings the frozen ui_smoke_test.cpp asserts through
// MainWindow::scanAgeLabelText. The extraction is behaviour-preserving only if
// they stay byte-identical here; the widget half (colour, tooltip, which frame)
// stays in MainWindow and is covered by that suite.

void testScanAgeText()
{
    const auto scanTime = QDateTime(QDate(2026, 5, 20), QTime(14, 22, 10), QTimeZone::UTC);

    // Fresh: two whole minutes, 1080 rays, no pattern parsed -> no VCP token.
    const auto fresh = cappedweather::scanAgeText(scanTime, 1080, 150);
    checkString(fresh.label, QStringLiteral("SCAN 14:22:10Z · 2 min old · 1080 rays"),
                "fresh label");
    check(!fresh.stale, "fresh label is not stale");
    check(!fresh.label.contains(QStringLiteral("VCP")),
          "fresh label carries no VCP token without a parsed pattern");

    // A pattern the decoder can vouch for prints between the time and the age.
    const auto withVcp = cappedweather::scanAgeText(scanTime, 1080, 150, 212);
    checkString(withVcp.label,
                QStringLiteral("SCAN 14:22:10Z · VCP 212 · 2 min old · 1080 rays"),
                "label with parsed VCP");
    check(!withVcp.stale, "VCP label at 2 min is not stale");

    // 0 and negative both mean "no vouchable pattern" and must not become a
    // token: a labelled-but-empty VCP reads like a value that failed to load.
    checkString(cappedweather::scanAgeText(scanTime, 1080, 150, 0).label, fresh.label,
                "pattern 0 renders the no-token form");
    checkString(cappedweather::scanAgeText(scanTime, 1080, 150, -1).label, fresh.label,
                "negative pattern renders the no-token form");

    // Stale: the same volume ten minutes later, flag and wording together.
    const auto stale = cappedweather::scanAgeText(scanTime, 720, 600);
    checkString(stale.label, QStringLiteral("SCAN 14:22:10Z · 10 min old · 720 rays"),
                "stale label");
    check(stale.stale, "stale label is flagged stale");

    // Unknown ray count: a spilled volume whose disk copy is unreadable.
    checkString(cappedweather::scanAgeText(scanTime, -1, 60).label,
                QStringLiteral("SCAN 14:22:10Z · 1 min old · — rays"),
                "unknown ray count label");

    // Unknown scan time: a frame whose header could not be read.
    checkString(cappedweather::scanAgeText(QDateTime(), 10, 60).label,
                QStringLiteral("SCAN — · 1 min old · 10 rays"), "unknown scan time label");
    const auto unknownBoth = cappedweather::scanAgeText(QDateTime(), -1, 60);
    checkString(unknownBoth.label, QStringLiteral("SCAN — · 1 min old · — rays"),
                "unknown time and rays together");

    // 0-minute clamp: a clock behind the scan time never prints "-1 min old".
    checkString(cappedweather::scanAgeText(scanTime, 10, -5).label,
                QStringLiteral("SCAN 14:22:10Z · 0 min old · 10 rays"),
                "negative age clamps to 0 min");
    checkString(cappedweather::scanAgeText(scanTime, 10, 59).label,
                QStringLiteral("SCAN 14:22:10Z · 0 min old · 10 rays"), "59 s floors to 0 min");
    // Whole minutes floor: 90 s is 1 minute, not 2.
    checkString(cappedweather::scanAgeText(scanTime, 10, 90).label,
                QStringLiteral("SCAN 14:22:10Z · 1 min old · 10 rays"), "90 s floors to 1 min");

    // Honest UTC: a scan time handed in with a +02:00 offset prints as UTC.
    const auto local =
        QDateTime(QDate(2026, 5, 20), QTime(16, 22, 10), QTimeZone::fromSecondsAheadOfUtc(7200));
    checkString(cappedweather::scanAgeText(local, 10, 60).label,
                QStringLiteral("SCAN 14:22:10Z · 1 min old · 10 rays"),
                "a non-UTC input renders as UTC");
}

void testScanAgeIsStale()
{
    check(cappedweather::kScanStaleSeconds == 300, "stale bound is 300 s");
    check(!cappedweather::scanAgeIsStale(299), "299 s is fresh");
    check(cappedweather::scanAgeIsStale(300), "300 s is stale (bound is inclusive)");
    check(cappedweather::scanAgeIsStale(301), "301 s is stale");
    check(!cappedweather::scanAgeIsStale(0), "0 s is fresh");
    check(!cappedweather::scanAgeIsStale(-1), "a negative age is never stale");
    check(!cappedweather::scanAgeIsStale(-3600), "an hour behind is never stale");
    check(cappedweather::scanAgeIsStale(600), "10 min is stale");
    check(cappedweather::scanAgeIsStale(86400), "a day is stale");

    // The label's flag must come from the same predicate, not a second copy of
    // the threshold: the boundary is where the two would drift apart.
    const auto scanTime = QDateTime(QDate(2026, 5, 20), QTime(14, 22, 10), QTimeZone::UTC);
    check(!cappedweather::scanAgeText(scanTime, 1, 299).stale, "label at 299 s is not stale");
    check(cappedweather::scanAgeText(scanTime, 1, 300).stale, "label at 300 s is stale");
    check(!cappedweather::scanAgeText(scanTime, 1, -30).stale, "label at -30 s is not stale");
}

void testScanAgeEmptyState()
{
    checkString(cappedweather::scanAgeEmptyStateText(),
                QStringLiteral("Waiting for latest scan"), "empty state text");
    checkString(cappedweather::scanAgeEmptyStateTooltip(),
                QStringLiteral("Pick a station to load the latest scan"), "empty state tooltip");

    const auto text = cappedweather::scanAgeEmptyStateText();
    const auto tip = cappedweather::scanAgeEmptyStateTooltip();
    // The block replaced the LIVE dot: it must not claim liveness even empty.
    check(!text.contains(QStringLiteral("LIVE"), Qt::CaseInsensitive),
          "empty text carries no LIVE wording");
    check(!tip.contains(QStringLiteral("LIVE"), Qt::CaseInsensitive),
          "empty tooltip carries no LIVE wording");
    // The frozen suite asserts the empty block still names the scan.
    check(text.contains(QStringLiteral("scan"), Qt::CaseInsensitive),
          "empty text names the scan");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    std::printf("cappedweather_ui_logic_tests: platform=%s\n",
                QGuiApplication::platformName().toUtf8().constData());

    testUiLogicTargetLinks();
    testScanAgeText();
    testScanAgeIsStale();
    testScanAgeEmptyState();

    if (failures != 0) {
        std::printf("ui logic tests failed: %d check(s)\n", failures);
        return 1;
    }
    std::printf("ui logic tests passed\n");
    return 0;
}
