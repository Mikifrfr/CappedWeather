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

    // Structural read of the brick the frame actually marched: occupied voxels,
    // how many of them were reconstructed, and the empty share of each column's
    // span. Reported twice: over the whole filled brick, and over the measured
    // voxels only. The second one is the shell defect as a number - it is what
    // the brick looked like before the vertical fill, on the same volume.
    const auto brick = canvas.volume3dBrick();
    std::size_t presentVoxels = 0;
    std::size_t measuredVoxels = 0;
    std::size_t interpolatedVoxels = 0;
    std::size_t spanColumns = 0;
    std::size_t spanLevels = 0;
    std::size_t emptyInSpan = 0;
    std::size_t measuredSpanLevels = 0;
    std::size_t measuredEmptyInSpan = 0;
    int interpolatedTilts = 0;
    if (brick) {
        measuredVoxels = brick->measuredVoxels;
        interpolatedVoxels = brick->interpolatedVoxels;
        interpolatedTilts = brick->interpolatedTilts;
        for (int iy = 0; iy < brick->ny; ++iy) {
            for (int ix = 0; ix < brick->nx; ++ix) {
                int low = -1;
                int high = -1;
                int measuredLow = -1;
                int measuredHigh = -1;
                std::size_t occupiedInSpan = 0;
                std::size_t measuredInSpan = 0;
                for (int iz = 0; iz < brick->nz; ++iz) {
                    const std::size_t flat = brickFlatIndex(*brick, ix, iy, iz);
                    if (brick->present[flat] == 0) continue;
                    ++presentVoxels;
                    if (low < 0) low = iz;
                    high = iz;
                    ++occupiedInSpan;
                    if (brickVoxelInterpolated(*brick, flat)) continue;
                    if (measuredLow < 0) measuredLow = iz;
                    measuredHigh = iz;
                    ++measuredInSpan;
                }
                if (low >= 0 && high - low >= 1) {
                    ++spanColumns;
                    spanLevels += static_cast<std::size_t>(high - low + 1);
                    emptyInSpan += static_cast<std::size_t>(high - low + 1) - occupiedInSpan;
                }
                if (measuredLow >= 0 && measuredHigh - measuredLow >= 1) {
                    measuredSpanLevels += static_cast<std::size_t>(measuredHigh - measuredLow + 1);
                    measuredEmptyInSpan += static_cast<std::size_t>(measuredHigh - measuredLow + 1) -
                                           measuredInSpan;
                }
            }
        }
    }
    const double columnGapFraction =
        spanLevels > 0 ? static_cast<double>(emptyInSpan) / static_cast<double>(spanLevels) : 0.0;
    const double measuredGapFraction = measuredSpanLevels > 0
                                           ? static_cast<double>(measuredEmptyInSpan) /
                                                 static_cast<double>(measuredSpanLevels)
                                           : 0.0;
    std::cout << "volume3d_probe=passed radar=" << decoded->radarId.toStdString()
              << " rays=" << decoded->decoded.rays.size()
              << " product=" << productName.toStdString()
              << " size=" << frame.width() << 'x' << frame.height()
              << " lit_fraction=" << litFraction << " checksum=" << checksum
              << " present_voxels=" << presentVoxels << " measured_voxels=" << measuredVoxels
              << " interpolated_voxels=" << interpolatedVoxels << " interpolated_tilts=" << interpolatedTilts
              << " span_columns=" << spanColumns << " column_gap_fraction=" << columnGapFraction
              << " measured_gap_fraction=" << measuredGapFraction
              << " build_ms=" << canvas.volume3dBuildMilliseconds()
              << " frame_ms=" << canvas.volume3dRenderMilliseconds()
              << " screenshot=" << outputPath.toStdString() << '\n';
    return 0;
}
