#pragma once

#include "core/level2_decoder.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <optional>

namespace cappedweather {

struct ArchiveLevel2Result {
    QString radarId;
    DecodedChunk decoded;
    int compressedRecords = 0;
    int failedRecords = 0;
    QStringList warnings;
};

struct AwsArchiveObject {
    QString key;
    QString radarId;
    QDateTime observedAt;
    qint64 size = 0;
};

std::optional<QByteArray> decompressLevel2BzipBlock(const QByteArray& input, QString* error = nullptr);
std::optional<ArchiveLevel2Result> decodeLevel2Archive(const QByteArray& bytes,
                                                       QString* error = nullptr,
                                                       const QString& radarIdHint = {});
QVector<AwsArchiveObject> parseAwsArchiveListing(const QByteArray& xml,
                                                 const QString& radarId,
                                                 QString* error = nullptr);
QVector<AwsArchiveObject> selectArchiveObjects(QVector<AwsArchiveObject> objects,
                                               const QDateTime& startUtc,
                                               const QDateTime& endUtc,
                                               int maximumVolumes = 0);
QUrl awsArchiveListingUrl(const QString& radarId, const QDate& utcDate);
QUrl awsArchiveObjectUrl(const QString& key);
QDateTime archiveTimeFromFileName(const QString& fileName);

} // namespace cappedweather
