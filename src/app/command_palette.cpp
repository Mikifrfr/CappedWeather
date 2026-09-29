#include "app/command_palette.hpp"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace cappedweather {

CommandPalette::CommandPalette(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("commandPalette"));
    setWindowTitle(QStringLiteral("Command palette"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(460, 380);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Type a radar, product, layout, or action… (Ctrl+K)"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("commandList"));
    list_->setUniformItemSizes(false);
    layout->addWidget(list_, 1);
    connect(search_, &QLineEdit::textChanged, this, [this] { refilter(); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { chooseCurrent(); });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!item) return;
        emit entryChosen(item->data(Qt::UserRole).toInt());
        close();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        emit entryChosen(item->data(Qt::UserRole).toInt());
        close();
    });
    search_->setFocus();
}

void CommandPalette::setEntries(QVector<Entry> entries) {
    entries_ = std::move(entries);
    list_->clear();
    for (int i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_.at(i);
        auto* item = new QListWidgetItem(entry.title, list_);
        item->setData(Qt::UserRole, i);
        if (!entry.detail.isEmpty()) item->setToolTip(entry.detail);
    }
    refilter();
}

void CommandPalette::refilter() {
    const auto query = search_->text().trimmed();
    int firstVisible = -1;
    for (int row = 0; row < list_->count(); ++row) {
        const auto* item = list_->item(row);
        const auto& entry = entries_.at(item->data(Qt::UserRole).toInt());
        const bool match =
            query.isEmpty() || entry.title.contains(query, Qt::CaseInsensitive) ||
            entry.detail.contains(query, Qt::CaseInsensitive);
        list_->setRowHidden(row, !match);
        if (match && firstVisible < 0) firstVisible = row;
    }
    if (firstVisible >= 0) list_->setCurrentRow(firstVisible);
}

void CommandPalette::chooseCurrent() {
    const auto* current = list_->currentItem();
    if (!current || list_->isRowHidden(list_->row(current))) {
        for (int row = 0; row < list_->count(); ++row) {
            if (!list_->isRowHidden(row)) {
                current = list_->item(row);
                break;
            }
        }
    }
    if (!current) return;
    emit entryChosen(current->data(Qt::UserRole).toInt());
    close();
}

} // namespace cappedweather
