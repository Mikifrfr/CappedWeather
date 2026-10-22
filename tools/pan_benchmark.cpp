// Pan benchmark: scripted drag-pans on live data to measure echo repaint cost.
//
// Opens the real MainWindow against live public NOAA endpoints, selects a
// radar, waits for a complete volume, lets the overlay build settle, then
// performs synthetic middle-button drag-pans on panel 0. Reports wall time per
// drag and the synchronous repaint cost of panel 0 (grab() forces a full paint).
//
// Usage: pan_benchmark [radar] [drags] [--with-3d] [--pair]
//
// `--with-3d` is the card t_f71a0be0 question: layout 1x2, panel 1 renders the
// 3D volume through the resident GPU march and is orbited while panel 0 pans, so
// the 2D repaint numbers describe a console with the 3D backend live. `--pair`
// is its control: the same 1x2 layout with panel 1 still on the 2D map. Without
// either flag the run is 1x1 and is the pre-3D-wiring baseline.
//
// No synthetic data is ever injected: without a live volume the tool fails.
#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QStandardPaths>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <cstdio>
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

double percentile(QVector<double> samples, const double fraction) {
    if (samples.isEmpty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const int index = std::clamp(static_cast<int>(fraction * (samples.size() - 1)), 0,
                                 static_cast<int>(samples.size()) - 1);
    return samples.at(index);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherPanBench"));
    QStandardPaths::setTestModeEnabled(true);

    QStringList arguments;
    for (int index = 1; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    const bool with3d = arguments.removeAll(QStringLiteral("--with-3d")) > 0;
    // The control for --with-3d: same 1x2 layout, second panel left on the 2D
    // map. Without it the only difference between the two runs would be panel 0's
    // width, and a narrower panel paints less -- that would look like a speedup
    // the 3D backend did not cause.
    const bool pair = arguments.removeAll(QStringLiteral("--pair")) > 0;
    const QString radar =
        arguments.size() > 0 ? arguments.at(0) : QStringLiteral("KJAX");
    const int drags = arguments.size() > 1 ? std::max(1, arguments.at(1).toInt()) : 3;

    MainWindow window;
    window.resize(1100, 750);
    window.show();
    window.move(10000, 500);

    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* layout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    auto maps = window.findChildren<RadarMapCanvas*>();
    std::sort(maps.begin(), maps.end(), [](const RadarMapCanvas* a, const RadarMapCanvas* b) {
        return a->objectName() < b->objectName();
    });
    if (!station || !layout || !status || maps.isEmpty()) {
        std::cerr << "benchmark could not find required UI controls\n";
        return 2;
    }
    // Single panel unless the 3D backend is under test: only panel 0 pans, so
    // the paint numbers belong to it either way.
    const bool pairedLayout = with3d || pair;
    layout->setCurrentIndex(pairedLayout ? 1 : 0);
    if (!selectRadar(station, radar)) return 3;
    if (!waitUntil(application, 360000, [&] {
            return maps[0]->radarId() == radar &&
                   status->text().contains(QStringLiteral("complete volume"),
                                           Qt::CaseInsensitive);
        })) {
        std::cerr << "volume timeout for " << radar.toStdString() << ": "
                  << status->text().toStdString() << '\n';
        return 4;
    }
    std::cout << "BENCH: volume ready, settling for overlay build\n";
    settle(application, 10000);

    // The 3D panel gets its own volume: activate it, select the same site again,
    // wait for its own march. If it never produces a frame, say so and stop
    // rather than reporting 2D numbers as if 3D had been running.
    RadarMapCanvas* volumePanel = nullptr;
    if (with3d && maps.size() > 1) {
        volumePanel = maps.at(1);
        volumePanel->activated();
        settle(application, 250);
        if (!selectRadar(station, radar)) return 3;
        if (!waitUntil(application, 360000, [&] {
                return volumePanel->radarId() == radar &&
                       status->text().contains(QStringLiteral("complete volume"),
                                               Qt::CaseInsensitive);
            })) {
            std::cerr << "3D panel volume timeout: " << status->text().toStdString() << '\n';
            return 6;
        }
        settle(application, 5000);
        volumePanel->setVolume3dMode(true);
        const bool framed = waitUntil(application, 120000, [&] {
            return volumePanel->volumeRenderMs() > 0.0;
        });
        std::printf("BENCH: 3d panel framed=%d backend=%s render_ms=%.1f gpu_error=\"%s\"\n",
                    framed ? 1 : 0,
                    volumePanel->volumeFrameGpu() ? "GPU" : "CPU",
                    volumePanel->volumeRenderMs(),
                    volumePanel->volumeGpuError().toStdString().c_str());
        if (!framed) {
            std::cerr << "3D panel produced no frame; refusing to report a 2D number as "
                         "'3D running'\n";
            return 7;
        }
    }
    const char* const modeName = with3d ? "2d+3d" : (pair ? "2d-pair" : "2d");
    std::cout << "BENCH: starting " << drags << " drags mode=" << modeName << '\n';
    if (!maps[0]->grab().save(QStringLiteral("C:/Users/brawl/AppData/Local/Temp/opencode/panbench_echo_before.png"),
                              "PNG")) {
        std::cerr << "could not save pre-drag echo grab\n";
        return 5;
    }
    std::cout << "BENCH: pre-drag echo grab saved\n";

    // Orbit drag held on the 3D panel so the GPU thread keeps marching while the
    // 2D panel paints; the grab() below is a full synchronous repaint of panel 0.
    QPoint orbitStart;
    if (volumePanel) {
        orbitStart = volumePanel->rect().center();
        QTest::mousePress(volumePanel, Qt::LeftButton, Qt::NoModifier, orbitStart);
    }
    const auto repaintMs = [&] {
        QElapsedTimer clock;
        clock.start();
        maps[0]->grab();
        return static_cast<double>(clock.nsecsElapsed()) / 1.0e6;
    };
    for (int warmup = 0; warmup < 3; ++warmup) {
        repaintMs();
        application.processEvents(QEventLoop::AllEvents, 20);
    }
    QVector<double> paintSamples;
    const QPoint start = maps[0]->rect().center();
    for (int drag = 0; drag < drags; ++drag) {
        QElapsedTimer dragClock;
        dragClock.start();
        // Middle-button drag: pans without risking a click on a polygon.
        QTest::mousePress(maps[0], Qt::MiddleButton, Qt::NoModifier, start);
        for (int step = 1; step <= 20; ++step) {
            QTest::mouseMove(maps[0], start + QPoint(step * 15, step * 8));
            if (volumePanel) {
                // A few pixels of orbit per step: a fast GPU frame each time.
                QTest::mouseMove(volumePanel, orbitStart + QPoint(step * 6, step * 2));
            }
            paintSamples.append(repaintMs());
            QTest::qWait(16);
        }
        QTest::mouseRelease(maps[0], Qt::MiddleButton, Qt::NoModifier,
                            start + QPoint(300, 160));
        settle(application, 2500); // let stale rebuilds drain before the next drag
        std::cout << "BENCH: drag " << drag << " wall=" << dragClock.elapsed() << "ms\n";
    }
    if (volumePanel) {
        QTest::mouseRelease(volumePanel, Qt::LeftButton, Qt::NoModifier,
                            orbitStart + QPoint(120, 40));
    }
    std::printf("PAINT: mode=%s samples=%lld median_ms=%.2f p95_ms=%.2f\n", modeName,
                static_cast<long long>(paintSamples.size()),
                percentile(paintSamples, 0.5), percentile(paintSamples, 0.95));
    if (volumePanel) {
        std::printf("PAINT: 3d backend=%s render_ms=%.1f after_orbit\n",
                    volumePanel->volumeFrameGpu() ? "GPU" : "CPU",
                    volumePanel->volumeRenderMs());
    }
    std::cout << "BENCH: done\n";
    if (!maps[0]->grab().save(QStringLiteral("C:/Users/brawl/AppData/Local/Temp/opencode/panbench_echo.png"),
                              "PNG")) {
        std::cerr << "could not save echo grab\n";
        return 5;
    }
    std::cout << "BENCH: echo grab saved\n";
    return 0;
}
