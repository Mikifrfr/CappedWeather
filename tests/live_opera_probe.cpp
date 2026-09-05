// Live OPERA end-to-end probe (anonymous S3, no credentials).
// Exercises listing -> download -> ODIM decode -> volume signals.
// Usage: cappedweather_live_opera_probe [ODIM_ID, default NOHUR]
#include "app/opera_source.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const QString radarId = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("NOHUR");
    OperaSource source;
    int exitCode = 3;
    bool sawChunk = false;
    std::size_t rayCount = 0;
    QObject::connect(
        &source, &OperaSource::chunkDecoded,
        [&](const QString&, const QString&, const QString&, const DecodedChunk& decoded,
            const QDateTime&) {
            const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
            std::size_t rays = 0;
            for (const auto& ray : decoded.rays) {
                if (ray.has(ProductKind::Reflectivity)) ++rays;
            }
            if (!decoded.rays.empty() && rays > 0) {
                sawChunk = true;
                rayCount = decoded.rays.size();
            }
            Q_UNUSED(index);
        });
    QObject::connect(
        &source, &OperaSource::volumeReady,
        [&](const QString& radar, const QString& volumeId, int chunks, int failed,
            const QDateTime& dataTime) {
            std::cout << "opera volume: radar=" << radar.toStdString()
                      << " volume=" << volumeId.toStdString() << " chunks=" << chunks
                      << " failed=" << failed
                      << " time=" << dataTime.toString(Qt::ISODate).toStdString()
                      << " rays=" << rayCount << '\n';
            exitCode = (sawChunk && failed == 0) ? 0 : 1;
            QCoreApplication::quit();
        });
    QObject::connect(&source, &OperaSource::requestFailed,
                     [&](const QString& radar, const QString& message) {
                         std::cerr << "opera request failed for " << radar.toStdString() << ": "
                                   << message.toStdString() << '\n';
                         exitCode = 2;
                         QCoreApplication::quit();
                     });
    QObject::connect(&source, &OperaSource::statusChanged, [](const QString& radar, const QString& s) {
        std::cout << "opera status [" << radar.toStdString() << "]: " << s.toStdString() << '\n';
    });
    QTimer::singleShot(0, [&] { source.requestLatest(radarId); });
    QTimer::singleShot(180000, [&] {
        std::cerr << "opera probe timed out\n";
        QCoreApplication::quit();
    });
    application.exec();
    return exitCode;
}
