// 3D volume probe: the measurement vehicle for both backends of the 3D view.
//
//   volume3d_probe <archive_V06> <out.png> [product] [settle_ms] [--gpu]
//
// Without --gpu it does what it always did: builds the panel's brick through
// RadarMapCanvas and grabs the CPU march. With --gpu it also renders the same
// brick, camera, transfer stops and opacity through the RHI raymarch, both
// windowed (the real VolumeCanvas widget) and offscreen, and prints the three
// lit_fraction numbers side by side. Same volume, same camera: the CPU/GPU
// comparison is the acceptance number for the GPU spike.

#include "app/archive_level2.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/volume_canvas.hpp"
#include "core/volume_brick.hpp"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>

using namespace cappedweather;

namespace {

struct FrameStats {
    int width = 0;
    int height = 0;
    double litFraction = 0.0;
    long long checksum = 0;
};

FrameStats statsFor(const QImage& frame) {
    FrameStats stats;
    if (frame.isNull()) return stats;
    stats.width = frame.width();
    stats.height = frame.height();
    int lit = 0;
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

void pump(QApplication& app, const int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

QSize parseSize(const QString& text, const QSize& fallback) {
    const auto parts = text.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if (parts.size() != 2) return fallback;
    bool okWidth = false;
    bool okHeight = false;
    const int width = parts.at(0).toInt(&okWidth);
    const int height = parts.at(1).toInt(&okHeight);
    if (!okWidth || !okHeight || width < 16 || height < 16) return fallback;
    return QSize(width, height);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication application(argc, argv);
    QStringList arguments = application.arguments();
    const bool gpu = arguments.removeAll(QStringLiteral("--gpu")) > 0;
    QSize size(1440, 900);
    for (int index = 0; index < arguments.size(); ++index) {
        if (arguments.at(index).startsWith(QStringLiteral("--size=")))
            size = parseSize(arguments.at(index).mid(7), size);
    }
    arguments.erase(std::remove_if(arguments.begin(), arguments.end(),
                                   [](const QString& value) {
                                       return value.startsWith(QStringLiteral("--size="));
                                   }),
                    arguments.end());
    if (arguments.size() < 3) {
        std::cerr << "usage: volume3d_probe <archive_V06> <out.png> [product] [settle_ms] "
                     "[--gpu] [--size=WxH]\n";
        return 2;
    }
    const QString archivePath = arguments.at(1);
    const QString outputPath = arguments.at(2);
    const QString productName = arguments.size() > 3 ? arguments.at(3) : QStringLiteral("ref");
    const int settleMs = arguments.size() > 4 ? arguments.at(4).toInt() : 6000;

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

    // The CPU preview marches a 256 x 256 x 128 brick with the panel's palette,
    // echo opacity and orbit defaults. Build the same brick here so the GPU
    // numbers describe the same volume, and drive the widget with the same
    // camera the canvas starts its 3D mode with (setVolume3dMode resets to
    // azimuth 180 / elevation 40 / 420 km / target z 7000 m).
    VolumeBrickDims dims;
    dims.nz = 128;
    const VolumeBrick brick = buildVolumeBrick(decoded->decoded, product, dims);
    std::size_t presentVoxels = 0;
    for (const auto flag : brick.present)
        if (flag) ++presentVoxels;
    const auto stops = RadarMapCanvas::volumeTransferStops(product, RadarMapCanvas::PaletteKind::Capped);
    const double target[3] = {brick.centerXMeters, brick.centerYMeters, 7000.0};
    const VolumeCamera camera = volumeCameraFor(brick, 180.0, 40.0, 420000.0, target);
    const int opacityPercent = 80; // RadarMapCanvas::radarOpacity_ default

    std::printf("volume3d_probe=passed radar=%s rays=%zu product=%s size=%dx%d "
                "brick=%dx%dx%d present_voxels=%zu voxel_m=%.1f\n",
                decoded->radarId.toStdString().c_str(), decoded->decoded.rays.size(),
                productName.toStdString().c_str(), size.width(), size.height(), brick.nx, brick.ny,
                brick.nz, presentVoxels, 2.0 * brick.halfExtentMeters / std::max(1, brick.nx));

    if (!gpu) {
        RadarMapCanvas canvas;
        canvas.setObjectName(QStringLiteral("radarMap"));
        canvas.resize(size);
        canvas.show();
        canvas.setDecodedChunk(decoded->decoded, QDateTime::currentDateTimeUtc());
        canvas.setProduct(product);
        application.processEvents();
        canvas.setVolume3dMode(true);
        pump(application, settleMs);
        const QImage frame = canvas.grab().toImage();
        if (frame.isNull() || !frame.save(outputPath, "PNG")) {
            std::cerr << "could not save " << outputPath.toStdString() << '\n';
            return 5;
        }
        const FrameStats stats = statsFor(frame);
        std::printf("cpu lit_fraction=%.5f checksum=%lld render_ms=%.1f screenshot=%s\n",
                    stats.litFraction, stats.checksum, canvas.volumeRenderMs(),
                    outputPath.toStdString().c_str());
        return 0;
    }

    // 2. Offscreen: no swapchain, no widget, just the march plus readback, which
    //    is the number the card is about.
    const VolumeGpuFrame offscreen = renderVolumeOffscreen(brick, camera, stops, opacityPercent,
                                                           size, 3);
    FrameStats offscreenStats;
    if (offscreen.ok) {
        const QString offscreenPath = outputPath + QStringLiteral("-offscreen.png");
        offscreen.image.save(offscreenPath, "PNG");
        offscreenStats = statsFor(offscreen.image);
        std::printf("gpu_offscreen backend=%s device=%s first_frame_ms=%.2f frame_ms=%.2f "
                    "lit_fraction=%.5f checksum=%lld screenshot=%s\n",
                    offscreen.backend.toStdString().c_str(), offscreen.device.toStdString().c_str(),
                    offscreen.firstFrameMs, offscreen.frameMs, offscreenStats.litFraction,
                    offscreenStats.checksum, offscreenPath.toStdString().c_str());
    } else {
        std::printf("gpu_offscreen failed error=\"%s\"\n", offscreen.error.toStdString().c_str());
    }

    // 1. The real widget, in a window, rendered to its color buffer.
    QWidget window;
    window.resize(size);
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* volumeCanvas = new VolumeCanvas(&window);
    layout->addWidget(volumeCanvas);
    volumeCanvas->setTransferStops(stops);
    volumeCanvas->setOrbit(180.0, 40.0, 420000.0, target);
    volumeCanvas->setOpacityPercent(opacityPercent);
    volumeCanvas->setVolume(std::make_shared<const VolumeBrick>(brick));
    window.show();
    QElapsedTimer wait;
    wait.start();
    while (volumeCanvas->frameCount() < 3 && wait.elapsed() < settleMs) {
        pump(application, 50);
    }
    FrameStats widgetStats;
    if (volumeCanvas->gpuReady()) {
        const QImage widgetFrame = volumeCanvas->grabFramebuffer();
        if (!widgetFrame.isNull()) {
            const QString widgetPath = outputPath + QStringLiteral("-widget.png");
            widgetFrame.save(widgetPath, "PNG");
            widgetStats = statsFor(widgetFrame.convertToFormat(QImage::Format_RGB32));
        }
    }
    std::printf("gpu_widget ready=%d backend=%s frames=%lld submit_ms=%.2f "
                "lit_fraction=%.5f status=\"%s\"\n",
                volumeCanvas->gpuReady() ? 1 : 0, volumeCanvas->backendName().toStdString().c_str(),
                static_cast<long long>(volumeCanvas->frameCount()), volumeCanvas->lastFrameMs(),
                widgetStats.litFraction, volumeCanvas->statusLine().toStdString().c_str());

    // 3. The CPU reference on the same volume and camera, through the shipping
    //    panel path.
    RadarMapCanvas canvas;
    canvas.setObjectName(QStringLiteral("radarMap"));
    canvas.resize(size);
    canvas.show();
    canvas.setDecodedChunk(decoded->decoded, QDateTime::currentDateTimeUtc());
    canvas.setProduct(product);
    application.processEvents();
    canvas.setVolume3dMode(true);
    pump(application, settleMs);
    const QImage cpuFrame = canvas.grab().toImage();
    if (!cpuFrame.isNull()) {
        const QString cpuPath = outputPath + QStringLiteral("-cpu.png");
        cpuFrame.save(cpuPath, "PNG");
    }
    const FrameStats cpuStats = statsFor(cpuFrame);
    std::printf("cpu lit_fraction=%.5f checksum=%lld render_ms=%.1f screenshot=%s\n",
                cpuStats.litFraction, cpuStats.checksum, canvas.volumeRenderMs(),
                (outputPath + QStringLiteral("-cpu.png")).toStdString().c_str());
    if (!offscreen.ok && !volumeCanvas->gpuReady()) {
        std::printf("volume3d_probe=gpu_unavailable error=\"%s\"\n",
                    offscreen.error.toStdString().c_str());
        return 0; // the CPU reference above is the fallback proof, not a failure
    }
    return 0;
}
