#pragma once

// M2 of the 3D volume design (docs/gpu-volume-rendering.md): the RHI raymarch
// backend. A QRhiWidget that takes the same VolumeBrick, the same
// VolumeCamera and the same transfer stops as the CPU march in
// RadarMapCanvas, so the two backends can be compared pixel for pixel.
//
// Scope of this spike: one widget, one shader set, D3D11. No menus, no
// product switching, no overlays. The CPU path is untouched and remains the
// fallback whenever the GPU side cannot start.

#include "app/volume_camera.hpp"
#include "core/volume_brick.hpp"

#include <QImage>
#include <QSize>
#include <QString>
#include <QRhiWidget>

#include <memory>
#include <vector>

namespace cappedweather {

// One completed offscreen frame: the march, the readback, and the wait for the
// GPU to finish (this is the honest "a frame costs this much" number, not the
// submit time). Used by tools/volume3d_probe and the hermetic test, both of
// which need GPU pixels without a window.
struct VolumeGpuFrame {
    QImage image;      // RGB32, already composited over the 3D view background
    double firstFrameMs = 0.0; // includes the 3D texture and LUT upload
    double frameMs = 0.0;      // completed frame with resources already resident
    bool ok = false;
    QString error;     // one line, suitable for a status chip
    QString backend;   // e.g. "Direct3D 11"
    QString device;    // e.g. "AMD Radeon RX 7800 XT"
};

// Offscreen raymarch: creates its own QRhi device, renders into a texture,
// reads it back and waits. Returns ok == false with a one-line error instead of
// throwing or returning a blank image with no explanation. `frames` repeats the
// march so `frameMs` describes a frame with the volume already uploaded.
// Used by tools/volume3d_probe and the hermetic test, which need GPU pixels
// without a window and must not disturb the panel's device.
VolumeGpuFrame renderVolumeOffscreen(const VolumeBrick& brick, const VolumeCamera& camera,
                                     const std::vector<RgbaStop>& stops, int opacityPercent,
                                     const QSize& pixelSize, int frames = 3);

// The panel's backend (card t_f71a0be0): the same march on a device that stays
// resident for the process, with the brick's 3D texture kept uploaded across
// frames. Blocking, so call it from a worker thread; returns ok == false with a
// one-line error when the device cannot start, the shader fails, or the GPU
// stops answering, and the caller (RadarMapCanvas) falls back to its CPU march.
// `frameMs` is the march plus readback of one frame; `firstFrameMs` is the whole
// call, so on a fresh device it carries the 91-132 ms device creation.
VolumeGpuFrame renderVolumeResident(std::shared_ptr<const VolumeBrick> brick,
                                    const VolumeCamera& camera,
                                    const std::vector<RgbaStop>& stops, int opacityPercent,
                                    const QSize& pixelSize);

// Frames the resident device has completed, and how many times it uploaded a
// brick. Proves a camera move reuses the resident texture instead of pushing
// 16.8 MB again (probe + test read these).
qint64 volumeResidentFrames();
qint64 volumeResidentUploads();



class VolumeCanvas : public QRhiWidget {
    Q_OBJECT

public:
    explicit VolumeCanvas(QWidget* parent = nullptr);
    ~VolumeCanvas() override;

    // The brick to render. Cheap: the 3D texture is only re-uploaded when the
    // pointer changes, so orbit frames reuse the resident texture.
    void setVolume(std::shared_ptr<const VolumeBrick> brick);
    // Transfer stops from RadarMapCanvas::volumeTransferStops(), so the 1D
    // palette LUT is the same colour ramp the CPU march and the 2D map use.
    void setTransferStops(std::vector<RgbaStop> stops);
    void setOrbit(double azimuthDeg, double elevationDeg, double distanceMeters,
                  const double target[3]);
    void setOpacityPercent(int percent);
    // 1 = full panel resolution, 2 = half (the CPU preview marches at 2).
    void setRenderScale(int divisor);

    [[nodiscard]] bool gpuReady() const;
    [[nodiscard]] QString statusLine() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] double lastFrameMs() const;
    [[nodiscard]] qint64 frameCount() const;
    [[nodiscard]] QSize renderSize() const;

signals:
    // Emitted once when the GPU path could not start, so the host can show the
    // reason and fall back to the CPU march instead of leaving a black panel.
    void gpuFailed(const QString& reason);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cappedweather
