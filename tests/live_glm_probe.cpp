#include "app/glm_source.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherLiveGlmProbe"));

    GlmSource source;
    QString latestStatus;
    QString latestError;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(&source, &GlmSource::statusChanged,
                     [&](const QString& status) {
        latestStatus = status;
        std::cout << "status=" << status.toStdString() << '\n';
    });
    QObject::connect(&source, &GlmSource::requestFailed,
                     [&](const QString& error) {
        latestError = error;
        std::cerr << "error=" << error.toStdString() << '\n';
    });
    QObject::connect(&source, &GlmSource::cellsUpdated,
                     [&](const QVector<LightningCellSnapshot>& cells,
                         const QDateTime& latestDataTime,
                         const int filesProcessed,
                         const int flashesProcessed) {
        if (filesProcessed <= 0 || flashesProcessed <= 0) return;
        std::cout << "live_glm_probe=passed"
                  << " files=" << filesProcessed
                  << " flashes=" << flashesProcessed
                  << " cells=" << cells.size()
                  << " latest=" << latestDataTime.toUTC().toString(Qt::ISODate).toStdString()
                  << '\n';
        for (const auto& cell : cells) {
            std::cout << "cell="
                      << cell.latitudeCenterDeg << ',' << cell.longitudeCenterDeg
                      << " sigma=" << cell.sigmaLevel
                      << " rate=" << cell.flashRatePerMin
                      << " active=" << (cell.active() ? "yes" : "no")
                      << '\n';
        }
        application.exit(0);
    });
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        std::cerr << "live_glm_probe=timed_out"
                  << " status=" << latestStatus.toStdString()
                  << " error=" << latestError.toStdString()
                  << '\n';
        application.exit(1);
    });

    timeout.start(120000);
    source.start();
    return application.exec();
}
