#include "app/s3_listing.hpp"

#include <QXmlStreamReader>

namespace cappedweather {

S3ListResult parseS3Listing(const QByteArray& data, QString* error) {
    if (error) error->clear();
    S3ListResult result;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNextStartElement();
        if (!xml.isStartElement()) continue;
        const auto name = xml.name();
        if (name == QStringLiteral("IsTruncated")) {
            result.truncated = xml.readElementText() == QStringLiteral("true");
        } else if (name == QStringLiteral("CommonPrefixes")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QStringLiteral("Prefix")) {
                    const auto prefix = xml.readElementText();
                    if (!prefix.isEmpty()) {
                        result.keys.push_back(prefix);
                        result.lastKey = prefix;
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
        } else if (name == QStringLiteral("Contents")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QStringLiteral("Key")) {
                    const auto key = xml.readElementText();
                    if (!key.isEmpty()) {
                        result.keys.push_back(key);
                        result.lastKey = key;
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
        }
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return {};
    }
    return result;
}

S3ObjectsResult parseS3Objects(const QByteArray& data, QString* error) {
    if (error) error->clear();
    S3ObjectsResult result;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNextStartElement();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QStringLiteral("IsTruncated")) {
            result.truncated = xml.readElementText() == QStringLiteral("true");
            continue;
        }
        if (xml.name() != QStringLiteral("Contents")) continue;

        QString key;
        QString modified;
        quint64 size = 0;
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("Key")) {
                key = xml.readElementText();
            } else if (xml.name() == QStringLiteral("LastModified")) {
                modified = xml.readElementText();
            } else if (xml.name() == QStringLiteral("Size")) {
                size = xml.readElementText().toULongLong();
            } else {
                xml.skipCurrentElement();
            }
        }
        if (!key.isEmpty()) result.lastKey = key;
        if (!key.isEmpty()) result.objects.push_back({key, size, modified});
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return {};
    }
    return result;
}

} // namespace cappedweather
