// 3D volume probe: the measurement vehicle for both backends of the 3D view.
//
//   volume3d_probe <archive_V06> <out.png> [product] [settle_ms] [--gpu] [--size=WxH]
//
// Without --gpu it builds the panel's brick through RadarMapCanvas and grabs the
// CPU march (the panel's explicit CPU backend), which is the number the 3D view
// had before the GPU wiring. With --gpu it renders the same brick, camera,
// transfer stops and opacity through:
//   gpu_offscreen  the spike's private-device path (also proves the widget's
//                  presentation path matches offscreen, via checksums)
//   gpu_widget     the real VolumeCanvas QRhiWidget in a window
//   gpu_resident   the panel's resident device: first frame vs resident frame
//   panel_gpu      the shipping panel with no backend override (the card's
//                  single switch), plus the wall time to the first frame
//   panel_gpu_settled
//                  the panel's second frame after a real orbit drag: the
//                  steady per-frame cost the chip shows while idle
//   cpu            the same panel forced to the CPU march
// Same volume, same camera: the CPU/GPU comparison is the acceptance number.

#include "app/archive_level2.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/volume_canvas.hpp"
#include "core/volume_brick.hpp"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
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
        // Explicit CPU: this run has always been the CPU reference, and the
        // panel's default backend is now the GPU.
        canvas.setVolume3dBackend(RadarMapCanvas::VolumeBackend::Cpu);
        application.processEvents();
        canvas.setVolume3dMode(true);
        pump(application, settleMs);
        const QImage frame = canvas.grab().toImage();
        if (frame.isNull() || !frame.save(outputPath, "PNG")) {
            std::cerr << "could not save " << outputPath.toStdString() << '\n';
            return 5;
        }
        const FrameStats stats = statsFor(frame);
        std::printf("cpu backend=%s lit_fraction=%.5f checksum=%lld render_ms=%.1f screenshot=%s\n",
                    canvas.volumeFrameGpu() ? "GPU" : "CPU", stats.litFraction, stats.checksum,
                    canvas.volumeRenderMs(), outputPath.toStdString().c_str());
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

    // 3. The resident device the panel actually uses (card t_f71a0be0): the
    //    first call pays device creation and the 16.8 MB upload, the second is
    //    the per-frame cost with both already resident.
    const auto brickNow = std::make_shared<const VolumeBrick>(brick);
    {
        const VolumeGpuFrame first =
            renderVolumeResident(brickNow, camera, stops, opacityPercent, size);
        const VolumeGpuFrame second =
            renderVolumeResident(brickNow, camera, stops, opacityPercent, size);
        if (first.ok && second.ok) {
            const FrameStats stats = statsFor(second.image);
            second.image.save(outputPath + QStringLiteral("-resident.png"), "PNG");
            std::printf("gpu_resident first_frame_ms=%.2f frame_ms=%.2f first_march_ms=%.2f "
                        "uploads=%lld frames=%lld lit_fraction=%.5f checksum=%lld device=%s\n",
                        first.firstFrameMs, second.frameMs, first.frameMs,
                        static_cast<long long>(volumeResidentUploads()),
                        static_cast<long long>(volumeResidentFrames()), stats.litFraction,
                        stats.checksum, first.device.toStdString().c_str());
        } else {
            const VolumeGpuFrame& bad = first.ok ? second : first;
            std::printf("gpu_resident failed error=\"%s\"\n", bad.error.toStdString().c_str());
        }
    }

    // 4. The shipping panel with no backend override: the single 3D toggle, the
    //    resident GPU march, the HUD and the chip. toggle_first_frame_ms is wall
    //    time from the toggle to the first published frame (brick build + device
    //    + march), because that is what the operator waits for.
    RadarMapCanvas gpuPanel;
    gpuPanel.setObjectName(QStringLiteral("radarMap"));
    gpuPanel.resize(size);
    gpuPanel.show();
    gpuPanel.setDecodedChunk(decoded->decoded, QDateTime::currentDateTimeUtc());
    gpuPanel.setProduct(product);
    application.processEvents();
    QElapsedTimer toggleClock;
    toggleClock.start();
    gpuPanel.setVolume3dMode(true);
    while (gpuPanel.volumeRenderMs() <= 0.0 && toggleClock.elapsed() < settleMs)
        pump(application, 25);
    const double toggleMs = static_cast<double>(toggleClock.elapsed());
    const QImage panelFrame = gpuPanel.grab().toImage();
    QString panelPath;
    FrameStats panelStats;
    if (!panelFrame.isNull()) {
        panelPath = outputPath + QStringLiteral("-panel-gpu.png");
        panelFrame.save(panelPath, "PNG");
        panelStats = statsFor(panelFrame);
    }
    std::printf("panel_gpu backend=%s render_ms=%.1f toggle_first_frame_ms=%.1f "
                "lit_fraction=%.5f checksum=%lld gpu_error=\"%s\" screenshot=%s\n",
                gpuPanel.volumeFrameGpu() ? "GPU" : "CPU", gpuPanel.volumeRenderMs(), toggleMs,
                panelStats.litFraction, panelStats.checksum,
                gpuPanel.volumeGpuError().toStdString().c_str(), panelPath.toStdString().c_str());

    // 4b. The same panel after one orbit. The first panel frame carries the
    //     pipeline compile for the panel's own target size (~30 ms here, the
    //     shader is compiled on the first draw after a resize), which is not the
    //     steady frame. Press, move, release exactly like an operator: the moves
    //     march at third resolution, the release refines at half resolution, and
    //     that refined frame is the number the chip shows while idle.
    {
        const qint64 framesBefore = volumeResidentFrames();
        const auto sendMouse = [&gpuPanel](const QEvent::Type type, const QPoint& point,
                                           const Qt::MouseButton button,
                                           const Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(point), QPointF(point),
                              QPointF(gpuPanel.mapToGlobal(point)), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&gpuPanel, &event);
        };
        const QPoint centre = gpuPanel.rect().center();
        sendMouse(QEvent::MouseButtonPress, centre, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 3; ++step) {
            sendMouse(QEvent::MouseMove, centre + QPoint(step * 20, step * 6), Qt::NoButton,
                      Qt::LeftButton);
            pump(application, 40);
        }
        sendMouse(QEvent::MouseButtonRelease, centre + QPoint(60, 18), Qt::LeftButton,
                  Qt::NoButton);
        QElapsedTimer clock;
        clock.start();
        // Two frames land after the release (the last fast move plus the refined
        // one); wait for them instead of guessing a sleep.
        while (volumeResidentFrames() < framesBefore + 2 && clock.elapsed() < settleMs)
            pump(application, 25);
        pump(application, 250); // let the publish land on the GUI thread
        const QImage settledFrame = gpuPanel.grab().toImage();
        if (!settledFrame.isNull())
            settledFrame.save(outputPath + QStringLiteral("-panel-gpu-settled.png"), "PNG");
        std::printf("panel_gpu_settled backend=%s render_ms=%.2f frames=%lld applied_ms=%.1f "
                    "lit_fraction=%.5f\n",
                    gpuPanel.volumeFrameGpu() ? "GPU" : "CPU", gpuPanel.volumeRenderMs(),
                    static_cast<long long>(volumeResidentFrames() - framesBefore),
                    static_cast<double>(clock.elapsed()), statsFor(settledFrame).litFraction);
    }

    // 5. The CPU reference on the same volume and camera, through the same
    //    shipping panel path and the same camera.
    RadarMapCanvas canvas;
    canvas.setObjectName(QStringLiteral("radarMap"));
    canvas.resize(size);
    canvas.show();
    canvas.setDecodedChunk(decoded->decoded, QDateTime::currentDateTimeUtc());
    canvas.setProduct(product);
    canvas.setVolume3dBackend(RadarMapCanvas::VolumeBackend::Cpu);
    application.processEvents();
    canvas.setVolume3dMode(true);
    pump(application, settleMs);
    const QImage cpuFrame = canvas.grab().toImage();
    if (!cpuFrame.isNull()) {
        const QString cpuPath = outputPath + QStringLiteral("-cpu.png");
        cpuFrame.save(cpuPath, "PNG");
    }
    const FrameStats cpuStats = statsFor(cpuFrame);
    std::printf("cpu backend=%s lit_fraction=%.5f checksum=%lld render_ms=%.1f screenshot=%s\n",
                canvas.volumeFrameGpu() ? "GPU" : "CPU", cpuStats.litFraction, cpuStats.checksum,
                canvas.volumeRenderMs(),
                (outputPath + QStringLiteral("-cpu.png")).toStdString().c_str());
    if (!offscreen.ok && !volumeCanvas->gpuReady() && !gpuPanel.volumeFrameGpu()) {
        std::printf("volume3d_probe=gpu_unavailable error=\"%s\"\n",
                    offscreen.error.toStdString().c_str());
        return 0; // the CPU reference above is the fallback proof, not a failure
    }
    return 0;
}
