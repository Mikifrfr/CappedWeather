// README screenshot capture: drives the real MainWindow against live public
// NOAA/NWS endpoints and grabs the two README screenshots. No synthetic
// data is ever injected: if live volumes or warning overlays cannot be
// fetched, the tool fails instead of faking a screenshot.
//
// Usage: readme_screenshots [radar]   (default: KABR)
// Outputs: docs/screenshots/cappedweather-console-2x2.png (2x2 console)
//          docs/screenshots/cappedweather-warnings.png (1x2, pinned warning)
// Run on the native platform (NOT offscreen: the offscreen QPA has no
// fonts, so all text renders as boxes). The window parks itself far
// off-screen; QWidget::grab repaints into a pixmap, so occlusion and
// position do not affect the capture.
#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QStandardPaths>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <iostream>

using namespace cappedweather;

namespace {

bool waitUntil(QApplication& application, const int timeoutMs, const auto& predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        application.processEvents(QEventLoop::AllEvents, 100);
        if (predicate()) return true;
        QThread::msleep(100);
    }
    application.processEvents(QEventLoop::AllEvents, 100);
    return predicate();
}

void settle(QApplication& application, int ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        application.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
}

bool selectRadar(QComboBox* station, const QString& radarId) {
    const int index = station->findData(radarId);
    if (index < 0) {
        std::cerr << "radar missing from picker: " << radarId.toStdString() << '\n';
        return false;
    }
    station->setCurrentIndex(index);
    station->activated(index);
    return true;
}

bool waitVolume(QApplication& application, RadarMapCanvas* map, QLabel* status,
                const QString& radarId, int timeoutMs) {
    return waitUntil(application, timeoutMs, [&] {
        return map->radarId() == radarId &&
               status->text().contains(QStringLiteral("complete volume"), Qt::CaseInsensitive);
    });
}

bool saveGrab(QWidget& window, const QString& path) {
    if (!window.grab().save(path, "PNG") || !QFileInfo(path).exists()) {
        std::cerr << "could not save " << path.toStdString() << '\n';
        return false;
    }
    std::cout << "saved " << path.toStdString() << '\n';
    return true;
}

// First inbox row carrying a real warning index (the trailing
// "plus N more" row has no UserRole data).
QListWidgetItem* firstWarningRow(QListWidget* inbox) {
    for (int i = 0; i < inbox->count(); ++i) {
        auto* item = inbox->item(i);
        const QVariant data = item->data(Qt::UserRole);
        if (data.isValid() && data.toInt() >= 0) return item;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherScreenshots"));
    QStandardPaths::setTestModeEnabled(true);

    const QString radar = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("KABR");

    MainWindow window;
    window.resize(1440, 850);
    window.show();
    window.move(10000, 500);

    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* layout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* presets = window.findChild<QComboBox*>(QStringLiteral("presetsCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    auto* inbox = window.findChild<QListWidget*>(QStringLiteral("warningList"));
    auto maps = window.findChildren<RadarMapCanvas*>();
    std::sort(maps.begin(), maps.end(), [](const RadarMapCanvas* a, const RadarMapCanvas* b) {
        return a->objectName() < b->objectName();
    });
    if (!station || !layout || !presets || !status || !inbox || maps.size() != 4) {
        std::cerr << "screenshot tool could not find required UI controls (maps="
                  << maps.size() << ")\n";
        return 2;
    }

    // --- Shot 1: 2x2 console. The built-in preset sets 2x2 with
    // REF/VEL/CC/ZDR across the four panels via MainWindow state.
    int consolePreset = -1;
    for (int i = 0; i < presets->count(); ++i) {
        if (presets->itemData(i).toString() == QStringLiteral("Analysis 2x2 dual-pol")) {
            consolePreset = i;
            break;
        }
    }
    if (consolePreset < 0) {
        std::cerr << "preset missing: Analysis 2x2 dual-pol\n";
        return 3;
    }
    presets->setCurrentIndex(consolePreset);
    if (!selectRadar(station, radar)) return 3;
    if (!waitVolume(application, maps[0], status, radar, 360000)) {
        std::cerr << "volume timeout for " << radar.toStdString() << ": "
                  << status->text().toStdString() << '\n';
        return 4;
    }
    settle(application, 4000);
    if (!saveGrab(window, QStringLiteral("docs/screenshots/cappedweather-console-2x2.png")))
        return 5;

    // --- Shot 2: 1x2 with a pinned NWS warning card. The built-in preset
    // sets 1x2 + REF/REF; the warning inbox (severity-sorted) is clicked
    // exactly like a user click, which flies the active panel there and
    // pins the card. The radar session from shot 1 is reused.
    int presetIndex = -1;
    for (int i = 0; i < presets->count(); ++i) {
        if (presets->itemData(i).toString() == QStringLiteral("Lightning scout 1x2")) {
            presetIndex = i;
            break;
        }
    }
    if (presetIndex < 0) {
        std::cerr << "preset missing: Lightning scout 1x2\n";
        return 6;
    }
    presets->setCurrentIndex(presetIndex);
    if (!waitUntil(application, 300000, [&] { return firstWarningRow(inbox) != nullptr; })) {
        std::cerr << "no NWS warnings arrived within timeout\n";
        return 7;
    }
    // Activate panel 0 explicitly; clear any stray card the click pins.
    QTest::mouseClick(maps[0], Qt::LeftButton, Qt::NoModifier, maps[0]->rect().center());
    settle(application, 500);
    maps[0]->clearPinnedOverlay();
    // Pin via the inbox with retries: warning geometry (especially
    // Meteoalarm tile-joined areas) resolves asynchronously, and
    // focusWarning silently skips warnings with no polygon yet. Re-fetch
    // the top row each round in case the inbox refreshes underneath.
    bool pinned = false;
    QString pinnedText;
    QElapsedTimer budget;
    budget.start();
    while (budget.elapsed() < 90000 && !pinned) {
        if (auto* row = firstWarningRow(inbox)) {
            pinnedText = row->text();
            inbox->itemClicked(row);
        }
        pinned = waitUntil(application, 8000,
                           [&] { return !maps[0]->infoCardRect().isNull(); });
    }
    settle(application, 1500);
    std::cout << "warning pinned=" << (pinned ? "yes" : "no");
    if (pinned) std::cout << " text=" << pinnedText.toStdString();
    std::cout << '\n';
    settle(application, 500);
    if (!saveGrab(window, QStringLiteral("docs/screenshots/cappedweather-warnings.png")))
        return 9;
    return pinned ? 0 : 8;
}
