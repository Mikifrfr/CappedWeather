#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Shared S3 ListObjectsV2 XML parsing. Callers keep their own pagination
// (start-after / truncated loops) and domain filtering; only the XML walk lives here.
namespace cappedweather {

struct S3ListResult {
    QStringList keys;
    bool truncated = false;
    QString lastKey;
};

// Collects every <Key> (inside <Contents>) and every <Prefix> (inside
// <CommonPrefixes>) in document order. For prefix-delimited listings keys holds
// the prefixes; for flat listings it holds object keys. lastKey is the last
// key/prefix seen and is suitable as the next start-after cursor.
S3ListResult parseS3Listing(const QByteArray& data, QString* error = nullptr);

struct S3ObjectEntry {
    QString key;
    quint64 size = 0;
    QString lastModified;
};

struct S3ObjectsResult {
    QVector<S3ObjectEntry> objects;
    bool truncated = false;
    QString lastKey;
};

// Detailed parse associating <Key>/<Size>/<LastModified> per <Contents> entry.
// lastKey is the last <Key> seen regardless of size so pagination cursors match
// the S3 ordering even when callers filter (e.g. "-I" chunks only).
S3ObjectsResult parseS3Objects(const QByteArray& data, QString* error = nullptr);

} // namespace cappedweather
