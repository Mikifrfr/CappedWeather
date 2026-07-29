#include "app/shortcut_help_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>

namespace cappedweather {

namespace {
struct ShortcutEntry { const char* key; const char* action; };
}

ShortcutHelpDialog::ShortcutHelpDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Keyboard shortcuts — CappedWeather"));
    setModal(true);
    resize(520, 480);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "CappedWeather keyboard shortcuts. Press <b>?</b> any time to reopen this dialog."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* text = new QTextEdit(this);
    text->setReadOnly(true);
    text->setStyleSheet(QStringLiteral(
        "QTextEdit { background: #1a1b1e; color: #d4d6d9; border: 1px solid #2b2d33; }"));
    populateShortcutsInto(text);
    layout->addWidget(text, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void ShortcutHelpDialog::populateShortcutsInto(QTextEdit* text) {
    const ShortcutEntry entries[] = {
        {"Space",        "Play / pause timeline"},
        {"Left / Right", "Previous / next frame"},
        {"PgUp / PgDn",  "Tilt up / down"},
        {"+ / -",        "Zoom in / out"},
        {"R",            "Recenter on selected radar"},
        {"L",            "Jump to LIVE (latest volume)"},
        {"1 - 8",        "Switch product (REF, VEL, SW, ZDR, CC, PHI, ET18, VIL)"},
        {"?",            "Show this help dialog"},
        {"Esc",          "Close this dialog"},
        {"F1",           "Show this help dialog"},
    };
    QString html = QStringLiteral("<table cellpadding='4' cellspacing='0' border='0'>");
    for (const auto& entry : entries) {
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                    .arg(QString::fromUtf8(entry.key), QString::fromUtf8(entry.action));
    }
    html += QStringLiteral("</table>");
    text->setHtml(html);
}

} // namespace cappedweather
