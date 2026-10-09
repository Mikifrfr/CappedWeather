// Dev/verification tool: downloads an offline basemap pack into the shared
// tile cache without opening the GUI. Same plan math and packer the File
// menu uses, so a passing run here proves the chase workflow end to end.
//
// Usage: offline_pack <lat> <lon> <halfDeg> <minZoom> <maxZoom>

#include "app/offline_pack.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("CappedWeather"));
    application.setOrganizationName(QStringLiteral("CappedWeather"));
    const auto args = application.arguments();
    if (args.size() != 6) {
        std::printf("usage: offline_pack <lat> <lon> <halfDeg> <minZoom> <maxZoom>\n");
        return 2;
    }
    bool ok = false;
    const double latitude = args.at(1).toDouble(&ok);
    if (!ok) return 2;
    const double longitude = args.at(2).toDouble(&ok);
    if (!ok) return 2;
    const double halfDegrees = args.at(3).toDouble(&ok);
    if (!ok) return 2;
    const int minZoom = args.at(4).toInt(&ok);
    if (!ok) return 2;
    const int maxZoom = args.at(5).toInt(&ok);
    if (!ok) return 2;
    const auto plan = cappedweather::planOfflinePack(latitude, longitude, halfDegrees, minZoom,
                                                     maxZoom);
    if (!plan.error.isEmpty()) {
        std::printf("plan rejected: %s\n", qPrintable(plan.error));
        return 1;
    }
    std::printf("packing %d tiles (%d urls, ~%.1f MB)...\n", static_cast<int>(plan.tiles.size()),
                static_cast<int>(plan.tiles.size()) * plan.urlsPerTile,
                plan.estimatedBytes / 1048576.0);
    auto* packer = new cappedweather::OfflinePacker(&application);
    QObject::connect(packer, &cappedweather::OfflinePacker::progressed,
                     [](int done, int total) {
                         if (done % 25 == 0 || done == total)
                             std::printf("  %d/%d\n", done, total);
                     });
    QObject::connect(packer, &cappedweather::OfflinePacker::finished,
                     [&application](bool success, const QString& message) {
                         std::printf("%s: %s\n", success ? "packed" : "failed",
                                     qPrintable(message));
                         application.exit(success ? 0 : 1);
                     });
    QTimer::singleShot(0, [packer, plan] { packer->start(plan); });
    // Ten minutes of grace for big packs; a stuck queue is a failure.
    QTimer::singleShot(600000, [&application] {
        std::printf("failed: timed out\n");
        application.exit(1);
    });
    return application.exec();
}
