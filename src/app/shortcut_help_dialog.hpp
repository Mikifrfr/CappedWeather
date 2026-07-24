#pragma once
#include <QDialog>

class QTextEdit;

namespace cappedweather {

class ShortcutHelpDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutHelpDialog(QWidget* parent = nullptr);

private:
    void populateShortcutsInto(QTextEdit* text);
};

} // namespace cappedweather
