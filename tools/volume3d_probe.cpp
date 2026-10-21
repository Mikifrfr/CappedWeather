#include "app/archive_level2.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPoint>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace cappedweather;

namespace {

// Pumps the event loop until the 3D worker has published a brick over `box`, or
// the budget runs out. A brick over a different box is a stale frame, not a
// result, so it never counts as ready.
bool waitForBox(RadarMapCanvas& canvas, const RadarMapCanvas::Volume3dBox& box,
                const int budgetMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < budgetMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        const auto brick = canvas.volume3dBrick();
        if (brick && std::fabs(brick->centerXMeters - box.centerXMeters) < 1.0 &&
            std::fabs(brick->centerYMeters - box.centerYMeters) < 1.0 &&
            std::fabs(brick->halfExtentMeters - box.halfExtentMeters) < 1.0) {
            return true;
        }
        QThread::msleep(20);
    }
    return false;
}

void settle(const int budgetMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < budgetMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    QApplication::processEvents();
}

struct FrameStats {
    double litFraction = 0.0;
    long long checksum = 0;
};

FrameStats frameStats(const QImage& frame) {
    FrameStats stats;
    if (frame.isNull()) return stats;
    long long lit = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb px = line[x];
            stats.checksum += qRed(px) * 3 + qGreen(px) * 5 + qBlue(px) * 7;
            if (qRed(px) + qGreen(px) + qBlue(px) > 90) ++lit;
        }
    }
    stats.litFraction =
        static_cast<double>(lit) / (static_cast<double>(frame.width()) * frame.height());
    return stats;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: volume3d_probe <archive_V06> <out.png> [product] [ms]\n"
                     "         [--box cx_km,cy_km,half_km | --box full]\n"
                     "         [--drag x0,y0,x1,y1]   (panel pixels, as Alt+left-drag)\n";
        return 2;
    }
    const QString archivePath = QString::fromLocal8Bit(argv[1]);
    const QString outputPath = QString::fromLocal8Bit(argv[2]);
    QString productName = QStringLiteral("ref");
    int settleMs = 6000;
    bool productSet = false;
    bool settleSet = false;
    bool boxRequested = false;
    bool dragRequested = false;
    RadarMapCanvas::Volume3dBox requestedBox;
    QPoint dragStart;
    QPoint dragEnd;
    for (int i = 3; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--box") && i + 1 < argc) {
            const QString spec = QString::fromLocal8Bit(argv[++i]);
            if (spec.compare(QStringLiteral("full"), Qt::CaseInsensitive) == 0) {
                boxRequested = false; // explicit full range: the default box
            } else {
                const auto parts = spec.split(',');
                if (parts.size() != 3) {
                    std::cerr << "bad --box, want cx_km,cy_km,half_km\n";
                    return 2;
                }
                requestedBox.centerXMeters = parts[0].toDouble() * 1000.0;
                requestedBox.centerYMeters = parts[1].toDouble() * 1000.0;
                requestedBox.halfExtentMeters = parts[2].toDouble() * 1000.0;
                boxRequested = true;
            }
        } else if (arg == QStringLiteral("--drag") && i + 1 < argc) {
            const auto parts = QString::fromLocal8Bit(argv[++i]).split(',');
            if (parts.size() != 4) {
                std::cerr << "bad --drag, want x0,y0,x1,y1\n";
                return 2;
            }
            dragStart = QPoint(parts[0].toInt(), parts[1].toInt());
            dragEnd = QPoint(parts[2].toInt(), parts[3].toInt());
            dragRequested = true;
        } else if (!productSet) {
            productName = arg;
            productSet = true;
        } else if (!settleSet) {
            settleMs = arg.toInt();
            settleSet = true;
        }
    }

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

    // The box path is the card's own gesture: --drag replays an Alt+left-drag
    // through the canvas, --box applies a box directly. Both are clamped by the
    // canvas, so what is printed below is the box that was actually built.
    bool dragMapped = true;
    if (dragRequested) {
        dragMapped = canvas.applyVolume3dBoxDrag(dragStart, dragEnd);
    } else if (boxRequested) {
        canvas.setVolume3dBox(requestedBox);
    }
    const auto box = canvas.volume3dBox();
    // A storm box marches far more of the screen than the full-range pancake
    // does, so the wait budget is generous: the published number is the brick's
    // own render time, not this budget.
    const bool brickReady = waitForBox(canvas, box, std::max(settleMs, 20000));
    settle(300);

    const QImage frame = canvas.grab().toImage();
    if (frame.isNull() || !frame.save(outputPath, "PNG")) {
        std::cerr << "could not save " << outputPath.toStdString() << '\n';
        return 5;
    }
    const FrameStats stats = frameStats(frame);

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
    // The acceptance as a number: the brick that was built is the box that was
    // asked for, within one voxel of its own grid.
    const double voxelMeters = brick ? 2.0 * brick->halfExtentMeters / std::max(1, brick->nx) : 0.0;
    const bool boxMatchesBrick =
        brick && std::fabs(brick->centerXMeters - box.centerXMeters) <= voxelMeters &&
        std::fabs(brick->centerYMeters - box.centerYMeters) <= voxelMeters &&
        std::fabs(brick->halfExtentMeters - box.halfExtentMeters) <= voxelMeters;

    std::cout << "volume3d_probe=passed radar=" << decoded->radarId.toStdString()
              << " rays=" << decoded->decoded.rays.size()
              << " product=" << productName.toStdString()
              << " size=" << frame.width() << 'x' << frame.height()
              << " lit_fraction=" << stats.litFraction << " checksum=" << stats.checksum
              << " present_voxels=" << presentVoxels << " measured_voxels=" << measuredVoxels
              << " interpolated_voxels=" << interpolatedVoxels
              << " interpolated_tilts=" << interpolatedTilts
              << " span_columns=" << spanColumns << " column_gap_fraction=" << columnGapFraction
              << " measured_gap_fraction=" << measuredGapFraction
              << " build_ms=" << canvas.volume3dBuildMilliseconds()
              << " frame_ms=" << canvas.volume3dRenderMilliseconds()
              << " roi=" << (dragRequested ? "drag" : boxRequested ? "box" : "full")
              << " drag_px=" << dragStart.x() << ',' << dragStart.y() << ',' << dragEnd.x() << ','
              << dragEnd.y() << " drag_mapped=" << (dragMapped ? 1 : 0)
              << " box_center_km=" << box.centerXMeters / 1000.0 << ',' << box.centerYMeters / 1000.0
              << " box_half_km=" << box.halfExtentMeters / 1000.0
              << " brick_center_km=" << (brick ? brick->centerXMeters / 1000.0 : 0.0) << ','
              << (brick ? brick->centerYMeters / 1000.0 : 0.0)
              << " brick_half_km=" << (brick ? brick->halfExtentMeters / 1000.0 : 0.0)
              << " voxel_m=" << voxelMeters << " box_matches_brick=" << (boxMatchesBrick ? 1 : 0)
              << " box_ready=" << (brickReady ? 1 : 0)
              << " coverage_km=" << canvas.volume3dCoverageMeters() / 1000.0
              << " screenshot=" << outputPath.toStdString() << '\n';
    if (!brickReady) {
        std::cerr << "the brick over the requested box never landed\n";
        return 6;
    }

    // Reset path, measured on the same real volume: full-range box, and the
    // render over it is not empty.
    if (dragRequested || boxRequested) {
        canvas.resetVolume3dBox();
        const auto resetBox = canvas.volume3dBox();
        const bool resetReady = waitForBox(canvas, resetBox, std::max(settleMs, 2000));
        settle(300);
        const FrameStats resetStats = frameStats(canvas.grab().toImage());
        std::cout << "volume3d_probe_reset=passed reset_box_full=" << (resetBox.fullRange() ? 1 : 0)
                  << " reset_box_half_km=" << resetBox.halfExtentMeters / 1000.0
                  << " reset_ready=" << (resetReady ? 1 : 0)
                  << " reset_lit_fraction=" << resetStats.litFraction
                  << " reset_checksum=" << resetStats.checksum
                  << " reset_frame_ms=" << canvas.volume3dRenderMilliseconds() << '\n';
        if (!resetReady || !resetBox.fullRange() || resetStats.litFraction <= 0.0) {
            std::cerr << "reset did not return to a non-empty full-range view\n";
            return 7;
        }
    }
    return 0;
}
