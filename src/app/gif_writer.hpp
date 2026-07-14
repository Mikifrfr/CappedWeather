#pragma once

#include <QImage>
#include <QString>
#include <QVector>

namespace cappedweather {

bool writeAnimatedGif(const QString& path, const QVector<QImage>& frames,
                      int delayCentiseconds, QString* error = nullptr);

} // namespace cappedweather
