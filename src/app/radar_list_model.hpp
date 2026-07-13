#pragma once

#include "core/radar_catalog.hpp"

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QVector>

namespace cappedweather {

class RadarListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        RadarIdRole = Qt::UserRole + 1,
        RadarNameRole,
        RadarOwnerRole,
        RadarStatusRole,
        RadarAgeRole,
    };

    explicit RadarListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    const RadarSite& siteAt(int row) const;
    std::vector<RadarSite> sites() const;
    int rowForRadar(const QString& radarId) const;
    void replaceSites(std::vector<RadarSite> sites);
    void setStatus(const QString& radarId, const QString& status, const QDateTime& receivedAt = {});

private:
    struct Status {
        QString text = QStringLiteral("unknown");
        QDateTime receivedAt;
    };

    QVector<RadarSite> sites_;
    QHash<QString, Status> status_;
};

} // namespace cappedweather
