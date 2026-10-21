#include "app/archive_level2.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: volume3d_probe <archive_V06> <out.png> [product] [ms]\n";
        return 2;
    }
    const QString archivePath = QString::fromLocal8Bit(argv[1]);
    const QString outputPath = QString::fromLocal8Bit(argv[2]);
    const QString productName = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("ref");
    const int settleMs = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 6000;

    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "cannot open " << archivePath.toStdString() << '\n';
        return 3;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    QString error;
    const auto decoded = decodeLevel2Archive(bytes, &error);
    if (!decoded) {
        std::cerr << "decode failed: " << error.toStdString() << '\n';
        return 4;
    }
    const auto product = productName.startsWith(QStringLiteral("vel")) ? ProductKind::Velocity
                                                                       : ProductKind::Reflectivity;

    RadarMapCanvas canvas;
    canvas.setObjectName(QStringLiteral("radarMap"));
    canvas.resize(1440, 900);
    canvas.show();
    canvas.setDecodedChunk(decoded->decoded, QDateTime::currentDateTimeUtc());
    canvas.setProduct(product);
    application.processEvents();
    canvas.setVolume3dMode(true);

    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < settleMs) {
        application.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    application.processEvents();

    const QImage frame = canvas.grab().toImage();
    if (frame.isNull() || !frame.save(outputPath, "PNG")) {
        std::cerr << "could not save " << outputPath.toStdString() << '\n';
        return 5;
    }
    int lit = 0;
    long long checksum = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb px = line[x];
            checksum += qRed(px) * 3 + qGreen(px) * 5 + qBlue(px) * 7;
            if (qRed(px) + qGreen(px) + qBlue(px) > 90) ++lit;
        }
    }
    const double litFraction =
        static_cast<double>(lit) / (static_cast<double>(frame.width()) * frame.height());
    std::cout << "volume3d_probe=passed radar=" << decoded->radarId.toStdString()
              << " rays=" << decoded->decoded.rays.size()
              << " product=" << productName.toStdString()
              << " size=" << frame.width() << 'x' << frame.height()
              << " lit_fraction=" << litFraction << " checksum=" << checksum
              << " screenshot=" << outputPath.toStdString() << '\n';
    return 0;
}
