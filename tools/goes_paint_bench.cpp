// GOES/HRRR paint bench: cold/warm repaint cost of one real frame through
// the real Qt renderer (offscreen). Not a test: prints p50/p95 grab timing,
// painted-cell counts (via CAPPEDWEATHER_PAINT_PROFILE), frame size and a
// screenshot checksum for before/after correctness comparisons.
//
// Usage: goes_paint_bench <rad.nc|refc.nc> [grabs]   (default grabs: 15)
#include "app/radar_map_canvas.hpp"
#include "core/goes_frame.hpp"
#include "core/hrrr_frame.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QStandardPaths>

#include <algorithm>
#include <iostream>
#include <vector>

using namespace cappedweather;

namespace {

struct View {
    const char* name;
    double lon;
    double lat;
    int zoom;
};

double percentile(std::vector<double> samples, const double q) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t i = std::min(samples.size() - 1,
                                   static_cast<std::size_t>(q * samples.size()));
    return samples[i];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: goes_paint_bench <goes_rad.nc> [grabs]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int grabs = argc > 2 ? std::max(3, atoi(argv[2])) : 15;

    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherGoesBench"));
    QStandardPaths::setTestModeEnabled(true);

    if (!goesReaderAvailable()) {
        std::cerr << "netCDF support unavailable\n";
        return 3;
    }
    // Frame kind follows the file: GOES Rad or HRRR REFC. HRRR reuses the
    // same views so raster costs compare directly.
    const auto goesFrame = readGoesRad(path);
    const auto hrrrFrame = !goesFrame.has_value() ? readHrrrRefc(path)
                                                  : std::optional<HrrrFrame>{};
    if (!goesFrame.has_value() && !hrrrFrame.has_value()) {
        std::cerr << "unreadable frame: " << path << '\n';
        return 4;
    }
    if (goesFrame.has_value()) {
        std::cout << "BENCH: frame nx=" << goesFrame->nx << " ny=" << goesFrame->ny
                  << " cells=" << (goesFrame->nx * goesFrame->ny)
                  << " channel=" << goesFrame->channel << '\n';
    } else {
        std::cout << "BENCH: frame nx=" << hrrrFrame->nx << " ny=" << hrrrFrame->ny
                  << " cells=" << (hrrrFrame->nx * hrrrFrame->ny) << " hrrr\n";
    }

    const View views[] = {
        {"national", -98.0, 38.0, 4},
        {"regional", -90.0, 35.0, 5},
        {"close", -86.0, 30.0, 7},
    };
    const QString saveDir = QString::fromLocal8Bit(
        qgetenv("GOES_BENCH_SAVE").constData());
    for (const auto& view : views) {
        RadarMapCanvas canvas;
        canvas.resize(1280, 800);
        canvas.show();
        application.processEvents();
        if (goesFrame.has_value()) {
            canvas.setGoesVisible(true);
            canvas.setGoesFrame(goesFrame);
        } else {
            canvas.setHrrrVisible(true);
            canvas.setHrrrFrame(hrrrFrame);
        }
        canvas.setView(QPointF(view.lon, view.lat), view.zoom);
        application.processEvents();

        QElapsedTimer coldClock;
        coldClock.start();
        const QPixmap cold = canvas.grab();
        const double coldMs = coldClock.nsecsElapsed() / 1e6;
        if (cold.isNull()) {
            std::cerr << "grab failed at " << view.name << '\n';
            return 5;
        }
        const QImage coldImage = cold.toImage();
        const quint16 coldSum = qChecksum(
            reinterpret_cast<const char*>(coldImage.constBits()),
            static_cast<uint>(coldImage.sizeInBytes()));

        std::vector<double> warm;
        for (int i = 0; i < grabs; ++i) {
            QElapsedTimer clock;
            clock.start();
            const QPixmap shot = canvas.grab();
            (void)shot;
            warm.push_back(clock.nsecsElapsed() / 1e6);
        }
        // Pan: shift the view one step and time the repaint.
        canvas.setView(QPointF(view.lon + 2.0, view.lat + 1.0), view.zoom);
        application.processEvents();
        QElapsedTimer panClock;
        panClock.start();
        const QPixmap pan = canvas.grab();
        const double panMs = panClock.nsecsElapsed() / 1e6;
        if (pan.isNull()) {
            std::cerr << "pan grab failed at " << view.name << '\n';
            return 6;
        }

        std::cout << "BENCH: view=" << view.name << " zoom=" << view.zoom
                  << " cold_ms=" << coldMs << " warm_p50_ms=" << percentile(warm, 0.5)
                  << " warm_p95_ms=" << percentile(warm, 0.95)
                  << " pan_ms=" << panMs << " checksum=" << coldSum << '\n';
        if (!saveDir.isEmpty()) {
            const QString prefix = goesFrame.has_value() ? QStringLiteral("/goes_")
                                                         : QStringLiteral("/hrrr_");
            cold.save(saveDir + prefix + QString::fromLatin1(view.name) +
                      QStringLiteral(".png"));
        }
    }
    std::cout << "BENCH: done\n";
    return 0;
}
