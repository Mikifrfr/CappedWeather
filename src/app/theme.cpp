#include "app/theme.hpp"

#include <QApplication>

namespace cappedweather {

QString appThemeName(const AppTheme theme) {
    switch (theme) {
        case AppTheme::Classic:
            return QStringLiteral("Classic");
        case AppTheme::Prototype:
            return QStringLiteral("Squall (prototype)");
    }
    return QStringLiteral("Classic");
}

AppTheme appThemeFromInt(const int value) {
    switch (value) {
        case 0:
            return AppTheme::Classic;
        case 1:
            return AppTheme::Prototype;
        default:
            return AppTheme::Classic;
    }
}

QColor appAccent(const AppTheme theme) {
    switch (theme) {
        case AppTheme::Classic:
            return QColor(128, 211, 255);
        case AppTheme::Prototype:
            return QColor(70, 200, 240);
    }
    return QColor(128, 211, 255);
}

QPalette appPalette(const AppTheme theme) {
    QPalette palette;
    if (theme == AppTheme::Classic) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#1a1b1e")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#d4d6d9")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#131416")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#1a1b1e")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#d4d6d9")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#212328")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#d4d6d9")));
        palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#1a1b1e")));
        palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#d4d6d9")));
        palette.setColor(QPalette::Highlight, QColor(126, 166, 201, 76));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#d4d6d9")));
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#5d636b")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#5d636b")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#5d636b")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#5d636b")));
        return palette;
    }
    // Squall prototype: cooler, deeper dark with a cyan signal accent.
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#14171d")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e9edf3")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#0d0f13")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#14171d")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#e9edf3")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#1b2028")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e9edf3")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#14171d")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#e9edf3")));
    palette.setColor(QPalette::Highlight, QColor(70, 200, 240, 90));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#0d0f13")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#5a6270")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#5a6270")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#5a6270")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#5a6270")));
    return palette;
}

namespace {

QString classicStylesheet() {
    return QStringLiteral(R"QSS(
QMainWindow { background: #131416; color: #d4d6d9; }
QDialog, QMessageBox, QFileDialog { background: #1a1b1e; color: #d4d6d9; }
QMainWindow::separator { background: #1a1b1e; width: 3px; height: 3px; }
QMainWindow::separator:hover { background: #34373d; }
QDockWidget { background: #1a1b1e; color: #9298a0; }
QDockWidget::title { background: #1a1b1e; color: #9298a0; border-bottom: 1px solid #2b2d33; padding: 7px 12px 6px; font-size: 10px; font-weight: 600; }
QWidget#dockBody { background: #1a1b1e; }
QWidget#mapHost { background: #131416; }
QScrollArea { background: #1a1b1e; border: none; }
QLabel { color: #d4d6d9; background: transparent; }
QLabel#section { color: #9298a0; font-size: 11px; }
QLabel#dockSection { color: #5d636b; font-size: 10px; font-weight: 600; padding-top: 4px; }
QToolButton#groupHeader { background: transparent; border: none; color: #5d636b; font-size: 10px; font-weight: 600; padding: 6px 0px 2px; }
QFrame#stationCard { background: transparent; border: none; }
QToolButton#productChip { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 6px 4px; }
QToolButton#productChip:hover { background: #282b31; border-color: #34373d; }
QToolButton#productChip:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QToolButton#productChip:disabled { color: #5d636b; background: #1a1b1e; border-color: #232529; }
QToolButton#tiltGridButton { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 4px 2px; }
QToolButton#tiltGridButton:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QLabel#layerBadge { color: #9298a0; font-size: 10px; padding: 0px 2px; }
QLabel#layerSub { color: #5d636b; font-size: 10px; padding: 0px 0px 2px 22px; }
QLabel#groupCaption { color: #5d636b; font-size: 10px; }
QComboBox, QLineEdit, QDoubleSpinBox, QSpinBox { background: #131416; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 4px 6px; selection-background-color: rgba(126, 166, 201, 76); selection-color: #d4d6d9; }
QComboBox:focus, QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus { border-color: #7ea6c9; }
QComboBox:disabled, QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #5d636b; border-color: #232529; background: #17181a; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QLineEdit { background: transparent; border: none; padding: 0px; }
QComboBox QAbstractItemView { background: #1a1b1e; color: #d4d6d9; border: 1px solid #2b2d33; outline: none; selection-background-color: rgba(126, 166, 201, 26); selection-color: #d4d6d9; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { background: #17181a; border-left: 1px solid #2b2d33; width: 16px; }
QToolButton, QPushButton { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 5px 4px; }
QToolButton:hover, QPushButton:hover { background: #282b31; border-color: #34373d; }
QToolButton:pressed, QPushButton:pressed { background: #131416; }
QToolButton:checked, QPushButton:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QToolButton:disabled, QPushButton:disabled { color: #5d636b; background: #1a1b1e; border-color: #232529; }
QToolButton#liveButton:checked { background: rgba(111, 191, 90, 26); border-color: #6fbf5a; color: #6fbf5a; }
QCheckBox { color: #d4d6d9; background: transparent; spacing: 6px; }
QCheckBox:disabled { color: #5d636b; }
QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #5d636b; background: transparent; }
QCheckBox::indicator:hover { border-color: #9298a0; }
QCheckBox::indicator:checked { border-color: #7ea6c9; background: #7ea6c9; }
QCheckBox::indicator:disabled { border-color: #232529; }
QSlider::groove:horizontal { background: #34373d; height: 2px; }
QSlider::sub-page:horizontal { background: #34373d; }
QSlider::add-page:horizontal { background: #34373d; }
QSlider::handle:horizontal { width: 12px; height: 12px; margin: -6px 0px; border-radius: 6px; background: #1a1b1e; border: 2px solid #7ea6c9; }
QProgressBar { background: #232529; border: none; max-height: 4px; }
QProgressBar::chunk { background: #7ea6c9; }
QStatusBar { background: #1a1b1e; color: #9298a0; border-top: 1px solid #2b2d33; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: #9298a0; background: transparent; font-size: 10px; }
QStatusBar QToolButton { padding: 2px 8px; }
QToolTip { background: #1a1b1e; color: #d4d6d9; border: 1px solid #2b2d33; padding: 4px 6px; }
QMenu { background: #1a1b1e; color: #d4d6d9; border: 1px solid #2b2d33; }
QMenu::item:selected { background: rgba(126, 166, 201, 26); }
QScrollBar:vertical { background: transparent; width: 5px; margin: 0px; }
QScrollBar::handle:vertical { background: #34373d; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #44484f; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background: transparent; height: 5px; margin: 0px; }
QScrollBar::handle:horizontal { background: #34373d; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #44484f; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; border: none; background: none; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
)QSS");
}

QString prototypeStylesheet() {
    return QStringLiteral(R"QSS(
QMainWindow { background: #0d0f13; color: #e9edf3; }
QDialog, QMessageBox, QFileDialog { background: #14171d; color: #e9edf3; }
QMainWindow::separator { background: #0d0f13; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: #2b3340; }
QDockWidget { background: #14171d; color: #8b94a5; }
QDockWidget::title { background: #10131a; color: #c7cfdb; border-bottom: 1px solid #262c36; padding: 8px 14px 7px; font-size: 10px; font-weight: 700; }
QWidget#dockBody { background: #14171d; }
QWidget#mapHost { background: #0d0f13; }
QScrollArea { background: #14171d; border: none; }
QLabel { color: #e9edf3; background: transparent; }
QLabel#section { color: #c7cfdb; font-size: 12px; font-weight: 600; }
QLabel#dockSection { color: #7d8698; font-size: 10px; font-weight: 700; padding-top: 10px; margin-top: 6px; border-top: 1px solid #232a35; }
QToolButton#groupHeader { background: transparent; border: none; color: #7d8698; font-size: 11px; font-weight: 700; padding: 8px 0px 4px; }
QToolButton#groupHeader:hover { color: #46c8f0; }
QFrame#stationCard { background: #10131a; border: 1px solid #262c36; border-radius: 10px; }
QToolButton#productChip { background: #0d0f13; color: #e9edf3; border: 1px solid #262c36; border-radius: 8px; padding: 7px 4px; }
QToolButton#productChip:hover { border-color: #39424f; }
QToolButton#productChip:checked { background: rgba(70, 200, 240, 14); border-color: #46c8f0; color: #46c8f0; }
QToolButton#productChip:disabled { color: #5a6270; background: #10131a; border-color: #1c212a; }
QToolButton#tiltGridButton { background: #0d0f13; color: #8b94a5; border: 1px solid #262c36; border-radius: 6px; padding: 5px 2px; }
QToolButton#tiltGridButton:hover { color: #e9edf3; border-color: #39424f; }
QToolButton#tiltGridButton:checked { background: rgba(70, 200, 240, 14); border-color: #46c8f0; color: #46c8f0; }
QLabel#layerBadge { color: #46c8f0; background: rgba(70, 200, 240, 14); border-radius: 10px; font-size: 10px; font-weight: 700; padding: 2px 9px; }
QLabel#layerSub { color: #5a6270; font-size: 10px; padding: 0px 0px 2px 24px; }
QLabel#groupCaption { color: #5a6270; font-size: 10px; }
QLabel#emptyStateHint { background: rgba(20, 23, 29, 230); border: 1px solid #46c8f0; border-radius: 10px; padding: 18px 26px; color: #e9edf3; font-size: 13px; }
QComboBox, QLineEdit, QDoubleSpinBox, QSpinBox, QDateTimeEdit { background: #0d0f13; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; padding: 5px 8px; selection-background-color: rgba(70, 200, 240, 90); selection-color: #0d0f13; }
QComboBox:focus, QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QDateTimeEdit:focus { border-color: #46c8f0; }
QComboBox:hover, QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover { border-color: #39424f; }
QComboBox:disabled, QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { color: #5a6270; border-color: #1c212a; background: #10131a; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QLineEdit { background: transparent; border: none; padding: 0px; }
QComboBox QAbstractItemView { background: #14171d; color: #e9edf3; border: 1px solid #262c36; outline: none; selection-background-color: rgba(70, 200, 240, 40); selection-color: #e9edf3; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { background: #10131a; border-left: 1px solid #262c36; width: 18px; }
QToolButton, QPushButton { background: #1b2028; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; padding: 6px 10px; }
QToolButton:hover, QPushButton:hover { background: #232b36; border-color: #39424f; }
QToolButton:pressed, QPushButton:pressed { background: #0d0f13; }
QToolButton:checked, QPushButton:checked { background: rgba(70, 200, 240, 32); border-color: #46c8f0; color: #46c8f0; }
QToolButton:disabled, QPushButton:disabled { color: #5a6270; background: #14171d; border-color: #1c212a; }
QToolButton#liveButton { font-weight: 700; padding: 6px 14px; }
QToolButton#liveButton:checked { background: rgba(123, 216, 138, 30); border-color: #7bd88a; color: #7bd88a; }
QCheckBox { color: #e9edf3; background: transparent; spacing: 7px; }
QCheckBox:disabled { color: #5a6270; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #5a6270; border-radius: 3px; background: transparent; }
QCheckBox::indicator:hover { border-color: #8b94a5; }
QCheckBox::indicator:checked { border-color: #46c8f0; background: #46c8f0; }
QCheckBox::indicator:disabled { border-color: #1c212a; }
QSlider::groove:horizontal { background: #262c36; height: 4px; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #46c8f0; border-radius: 2px; }
QSlider::add-page:horizontal { background: #262c36; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0px; border-radius: 7px; background: #0d0f13; border: 2px solid #46c8f0; }
QSlider::handle:horizontal:hover { background: rgba(70, 200, 240, 40); }
QProgressBar { background: #1c212a; border: none; border-radius: 2px; max-height: 5px; text-align: center; }
QProgressBar::chunk { background: #46c8f0; border-radius: 2px; }
QStatusBar { background: #10131a; color: #8b94a5; border-top: 1px solid #232a35; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: #8b94a5; background: transparent; font-size: 10px; }
QStatusBar QToolButton { padding: 3px 10px; }
QToolTip { background: #14171d; color: #e9edf3; border: 1px solid #46c8f0; border-radius: 4px; padding: 5px 8px; }
QMenu { background: #14171d; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; padding: 4px; }
QMenu::item { border-radius: 4px; padding: 5px 22px 5px 12px; }
QMenu::item:selected { background: rgba(70, 200, 240, 40); }
QMenu::separator { height: 1px; background: #232a35; margin: 4px 8px; }
QGroupBox { color: #c7cfdb; font-weight: 700; border: 1px solid #232a35; border-radius: 8px; margin-top: 14px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #7d8698; }
QScrollBar:vertical { background: transparent; width: 7px; margin: 0px; }
QScrollBar::handle:vertical { background: #2b3340; min-height: 28px; border-radius: 3px; }
QScrollBar::handle:vertical:hover { background: #46c8f0; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background: transparent; height: 7px; margin: 0px; }
QScrollBar::handle:horizontal { background: #2b3340; min-width: 28px; border-radius: 3px; }
QScrollBar::handle:horizontal:hover { background: #46c8f0; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; border: none; background: none; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
)QSS");
}

} // namespace

QString appStylesheet(const AppTheme theme) {
    return theme == AppTheme::Prototype ? prototypeStylesheet() : classicStylesheet();
}

} // namespace cappedweather
