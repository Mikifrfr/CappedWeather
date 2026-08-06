#pragma once

#include "app/placefile.hpp"

#include <QObject>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

#include <map>
#include <memory>

class QNetworkReply;
class QTimer;

namespace cappedweather {

class PlacefileSource final : public QObject {
    Q_OBJECT

public:
    explicit PlacefileSource(QObject* parent = nullptr);

    bool addLocalFile(const QString& path, QString* error = nullptr);
    bool addUrl(const QUrl& url, QString* error = nullptr);
    void setContext(double latitude, double longitude);
    void clear();
    QVector<PlacefileDocument> documents() const;
    int sourceCount() const;

signals:
    void documentsChanged(QVector<cappedweather::PlacefileDocument> documents);
    void statusChanged(QString status);

private:
    struct Entry {
        QString key;
        QUrl url;
        bool remote = false;
        std::optional<PlacefileDocument> document;
        QTimer* refreshTimer = nullptr;
        QPointer<QNetworkReply> reply;
    };

    void request(Entry& entry);
    void finish(QNetworkReply* reply);
    void publish();

    QNetworkAccessManager network_;
    std::map<QString, std::unique_ptr<Entry>> entries_;
    QHash<QNetworkReply*, QString> replies_;
    double contextLatitude_ = 0.0;
    double contextLongitude_ = 0.0;
};

} // namespace cappedweather
