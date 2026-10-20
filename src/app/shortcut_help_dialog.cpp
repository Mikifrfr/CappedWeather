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
    text->setObjectName(QStringLiteral("shortcutList"));
    text->setReadOnly(true);
    // Colors come from the active theme sheet. A hardcoded inline sheet pinned
    // this dialog to Classic grays permanently, so it looked like a different
    // app under the Squall theme.
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
        {"Home / End",   "Oldest / newest frame"},
        {"PgUp / PgDn",  "Tilt up / down"},
        {"+ / -",        "Zoom in / out"},
        {"R",            "Recenter on selected radar"},
        {"L",            "Jump to latest volume (follow newest scan)"},
        {"1 - 9, 0",      "Switch product: 1 REF, 2 VEL, 3 SW, 4 ZDR, 5 CC, 6 PHI, 7 ET18, 8 VIL, 9 CREF, 0 CAPPI"},
        {"P",            "Lock / unlock pinned card"},
        {"Up / Down",    "Scroll pinned card (when it overflows)"},
        {"B",            "Bookmark / unbookmark current frame"},
        {"Ctrl+K",       "Command palette (radar, product, layout, action)"},
        {"Double-click card", "Expand / collapse the pinned detail card"},
        {"Ctrl+Z / Ctrl+Shift+Z", "Undo / redo map view"},
        {"F11",          "Maximize / restore active panel"},
        {"Esc",          "Close card, cancel measure / cross-section"},
        {"?",            "Show this help dialog"},
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
