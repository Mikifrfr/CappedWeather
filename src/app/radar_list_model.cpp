#include "app/radar_list_model.hpp"

#include <QVariant>

#include <utility>

namespace cappedweather {

RadarListModel::RadarListModel(QObject* parent)
    : QAbstractListModel(parent) {
    const auto sites = RadarCatalog::defaultSites();
    sites_.reserve(static_cast<qsizetype>(sites.size()));
    for (const auto& site : sites) {
        sites_.push_back(site);
        status_.insert(QString::fromStdString(site.id), {});
    }
    // European OPERA sites ride the same model, picker, and panels.
    for (const auto& site : RadarCatalog::operaSites()) {
        sites_.push_back(site);
        status_.insert(QString::fromStdString(site.id), {});
    }
}

int RadarListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : sites_.size();
}

QVariant RadarListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= sites_.size()) {
        return {};
    }

    const auto& site = sites_.at(index.row());
    const auto id = QString::fromStdString(site.id);
    const auto state = status_.value(id);

    switch (role) {
    case Qt::DisplayRole:
    case RadarIdRole:
        return id;
    case RadarNameRole:
        return QString::fromStdString(site.name);
    case RadarOwnerRole:
        return QString::fromStdString(site.owner);
    case RadarStatusRole:
        return state.text;
    case RadarAgeRole:
        return state.receivedAt.isValid() ? state.receivedAt.toString(Qt::ISODate) : QString{};
    default:
        return {};
    }
}

QHash<int, QByteArray> RadarListModel::roleNames() const {
    return {
        {RadarIdRole, "radarId"},
        {RadarNameRole, "radarName"},
        {RadarOwnerRole, "radarOwner"},
        {RadarStatusRole, "radarStatus"},
        {RadarAgeRole, "radarAge"},
    };
}

const RadarSite& RadarListModel::siteAt(const int row) const {
    return sites_.at(row);
}

std::vector<RadarSite> RadarListModel::sites() const {
    return {sites_.cbegin(), sites_.cend()};
}

int RadarListModel::rowForRadar(const QString& radarId) const {
    for (int row = 0; row < sites_.size(); ++row) {
        if (QString::fromStdString(sites_.at(row).id) == radarId) {
            return row;
        }
    }
    return -1;
}

void RadarListModel::replaceSites(std::vector<RadarSite> sites) {
    beginResetModel();
    sites_.clear();
    status_.clear();
    sites_.reserve(static_cast<qsizetype>(sites.size()));
    for (auto& site : sites) {
        const auto id = QString::fromStdString(site.id);
        if (!status_.contains(id)) {
            status_.insert(id, Status{});
        }
        sites_.push_back(std::move(site));
    }
    endResetModel();
}

void RadarListModel::setStatus(const QString& radarId, const QString& status, const QDateTime& receivedAt) {
    auto& state = status_[radarId];
    state.text = status;
    if (receivedAt.isValid()) {
        state.receivedAt = receivedAt;
    }

    for (int row = 0; row < sites_.size(); ++row) {
        if (QString::fromStdString(sites_.at(row).id) == radarId) {
            emit dataChanged(index(row), index(row), {RadarStatusRole, RadarAgeRole});
            return;
        }
    }
}

} // namespace cappedweather
