#pragma once

#include <QString>
#include <QVector>
#include <QDialog>

class QLineEdit;
class QListWidget;

namespace cappedweather {

// Ctrl+K command palette: fuzzy-ish filter over radars, products,
// layouts, and actions. Modeless, closes on choose or Esc.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    struct Entry {
        QString title;
        QString detail;
    };

    explicit CommandPalette(QWidget* parent = nullptr);
    void setEntries(QVector<Entry> entries);

signals:
    void entryChosen(int index);

private:
    void refilter();
    void chooseCurrent();

    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    QVector<Entry> entries_;
};

} // namespace cappedweather
