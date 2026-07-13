#pragma once

#include "core/product.hpp"
#include "core/level2_decoder.hpp"

#include <QDateTime>
#include <QImage>
#include <QWidget>

#include <optional>

class QPainter;

namespace cappedweather {

class RadarCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit RadarCanvas(QWidget* parent = nullptr);

    void setRadarId(const QString& radarId);
    void setProduct(ProductKind product);
    void setElevationFilter(std::optional<float> elevationDegrees);
    void setStatus(const QString& status);
    void setRawObjectInfo(const QString& key, qsizetype bytes, const QDateTime& receivedAt);
    void setDecodedChunk(DecodedChunk chunk);
    QString radarId() const;
    ProductKind product() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintRadarFrame(QPainter& painter, const QRectF& plot);
    void paintEchoes(QPainter& painter, const QRectF& plot);

    QString radarId_ = QStringLiteral("----");
    QString status_ = QStringLiteral("No live data received");
    QString objectKey_;
    qsizetype objectBytes_ = 0;
    QDateTime receivedAt_;
    ProductKind product_ = ProductKind::Reflectivity;
    std::optional<float> elevationFilter_;
    DecodedChunk chunk_;
};

} // namespace cappedweather
