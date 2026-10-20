#include "app/theme.hpp"

#include <QApplication>

#include <algorithm>

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
        // Secondary/dimmed text. #5d636b only reached 2.8:1 on the dock body,
        // so the station-picker placeholder, every group caption and every
        // disabled control were effectively unreadable. #7d838c clears 4.5:1.
        palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#7d838c")));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#7d838c")));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#7d838c")));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#7d838c")));
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
    // See the Classic note above: #5a6270 was 2.9:1 on the dock body.
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#828b9b")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#828b9b")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#828b9b")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#828b9b")));
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
QToolButton#groupHeader { background: transparent; border: none; border-top: 1px solid #232529; color: #7d838c; font-size: 10px; font-weight: 600; padding: 8px 0px 2px; }
QToolButton#groupHeader:hover { color: #d4d6d9; }
QToolButton#groupHeader:focus { color: #7ea6c9; }
QFrame#stationCard { background: transparent; border: none; }
QToolButton#productChip { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 6px 4px; }
QToolButton#productChip:hover { background: #282b31; border-color: #34373d; }
QToolButton#productChip:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QToolButton#productChip:disabled { color: #7d838c; background: #17181a; border-color: #4a4e57; }
QToolButton#tiltGridButton { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 4px 2px; }
QToolButton#tiltGridButton:hover { background: #282b31; border-color: #34373d; }
QToolButton#tiltGridButton:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QLabel[layerBadge="true"] { color: #9298a0; background: rgba(126, 166, 201, 22); border-radius: 3px; font-size: 10px; font-weight: 600; padding: 1px 6px; }
QLabel#layerSub { color: #7d838c; font-size: 10px; padding: 0px 0px 2px 22px; }
QLabel#groupCaption { color: #7d838c; font-size: 10px; }
QLabel#activeProductLabel { color: #d4d6d9; font-size: 12px; font-weight: 600; padding-bottom: 2px; }
QLabel#emptyStateHint { color: #e4e9ee; background: rgba(19, 20, 22, 235); border: 1px solid #7ea6c9; border-radius: 8px; padding: 14px 18px; }
QLabel#offlineBanner { color: #131416; background: #e0c14a; border-radius: 6px; padding: 6px 12px; font-weight: 700; }
QLabel#glmStatusNote[notice="true"] { color: #ff9a8f; font-size: 10px; }
QComboBox, QLineEdit, QDoubleSpinBox, QSpinBox, QDateTimeEdit { background: #131416; color: #d4d6d9; border: 1px solid #3f444d; border-radius: 0px; padding: 4px 6px; selection-background-color: rgba(126, 166, 201, 76); selection-color: #d4d6d9; }
QComboBox:hover, QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QDateTimeEdit:hover { border-color: #4d525c; }
QComboBox:focus, QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QDateTimeEdit:focus { border: 1px solid #7ea6c9; }
QComboBox:disabled, QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QDateTimeEdit:disabled { color: #7d838c; border-color: #4a4e57; background: #17181a; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::drop-down:hover { background: #282b31; }
QComboBox QLineEdit { background: transparent; border: none; padding: 0px; }
QComboBox QAbstractItemView { background: #1a1b1e; color: #d4d6d9; border: 1px solid #3f444d; outline: none; selection-background-color: rgba(126, 166, 201, 90); selection-color: #d4d6d9; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { background: #17181a; border-left: 1px solid #2b2d33; width: 16px; }
QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover, QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #282b31; }
QToolButton, QPushButton { background: #212328; color: #d4d6d9; border: 1px solid #2b2d33; border-radius: 0px; padding: 5px 4px; }
QToolButton:hover, QPushButton:hover { background: #282b31; border-color: #34373d; }
QToolButton:pressed, QPushButton:pressed { background: #131416; }
QToolButton:focus, QPushButton:focus { border: 1px solid #7ea6c9; }
QToolButton:checked, QPushButton:checked { background: rgba(126, 166, 201, 26); border-color: #7ea6c9; color: #7ea6c9; }
QToolButton:disabled, QPushButton:disabled { color: #7d838c; background: #17181a; border-color: #4a4e57; }
QCheckBox { color: #d4d6d9; background: transparent; spacing: 6px; }
QCheckBox:disabled { color: #7d838c; }
QCheckBox:focus { color: #7ea6c9; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #7d838c; background: transparent; }
QCheckBox::indicator:hover { border-color: #9298a0; }
QCheckBox::indicator:checked { border-color: #7ea6c9; background: #7ea6c9; }
QCheckBox::indicator:disabled { border-color: #4a4e57; background: transparent; }
QCheckBox::indicator:focus { border: 2px solid #7ea6c9; }
QSlider::groove:horizontal { background: #34373d; height: 3px; }
QSlider::sub-page:horizontal { background: #7ea6c9; }
QSlider::add-page:horizontal { background: #34373d; }
QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0px; border-radius: 6px; background: #1a1b1e; border: 2px solid #7ea6c9; }
QSlider::handle:horizontal:hover { background: #7ea6c9; }
QSlider::handle:horizontal:focus { background: #7ea6c9; }
QSlider::groove:horizontal:disabled, QSlider::sub-page:horizontal:disabled, QSlider::add-page:horizontal:disabled { background: #4a4e57; }
QSlider::handle:horizontal:disabled { border: 2px solid #4a4e57; background: #17181a; }
QProgressBar { background: #232529; border: none; height: 4px; }
QProgressBar::chunk { background: #7ea6c9; }
QStatusBar { background: #1a1b1e; color: #9298a0; border-top: 1px solid #2b2d33; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: #9298a0; background: transparent; }
QStatusBar QToolButton { padding: 2px 8px; }
QToolTip { background: #1a1b1e; color: #d4d6d9; border: 1px solid #3f444d; padding: 4px 6px; }
QTextEdit#shortcutList { background: #131416; color: #d4d6d9; border: 1px solid #3f444d; selection-background-color: rgba(126, 166, 201, 90); }
QTabWidget#sidebarTabs::pane { border: none; background: transparent; }
QTabBar::tab { background: #1a1b1e; color: #9298a0; border: 1px solid #2b2d33; border-bottom: none; padding: 6px 4px; font-size: 10px; font-weight: 600; }
QTabBar::tab:selected { background: #212328; color: #7ea6c9; border-color: #3f444d; }
QTabBar::tab:hover:!selected { color: #d4d6d9; }
QTabBar::tab:focus { border: 1px solid #7ea6c9; }
QTabBar#productTabBar { background: #131416; }
QTabBar#productTabBar::tab { padding: 6px 10px; }
QTabBar#productTabBar::tab:disabled { color: #7d838c; background: #17181a; }
QListWidget#warningList { background: #131416; color: #d4d6d9; border: 1px solid #3f444d; outline: none; font-size: 11px; }
QListWidget#warningList::item { padding: 5px 6px; border-bottom: 1px solid #232529; }
QListWidget#warningList::item:hover { background: rgba(126, 166, 201, 26); }
QListWidget#warningList::item:selected { background: rgba(126, 166, 201, 60); color: #d4d6d9; }
QListWidget#warningList:focus { border: 1px solid #7ea6c9; }
QListWidget#commandList { background: #1a1b1e; color: #d4d6d9; border: 1px solid #2b2d33; outline: none; }
QListWidget#commandList::item { padding: 5px 8px; }
QListWidget#commandList::item:selected { background: rgba(126, 166, 201, 60); color: #d4d6d9; }
QMenuBar { background: #1a1b1e; color: #d4d6d9; border: none; }
QMenuBar::item { background: transparent; padding: 4px 9px; }
QMenuBar::item:selected { background: rgba(126, 166, 201, 26); }
QMenuBar::item:pressed { background: rgba(126, 166, 201, 45); }
QMenuBar::item:focus { background: rgba(126, 166, 201, 26); color: #7ea6c9; }
QMenu { background: #1a1b1e; color: #d4d6d9; border: 1px solid #3f444d; }
QMenu::item { padding: 5px 22px 5px 12px; }
QMenu::item:selected { background: rgba(126, 166, 201, 90); color: #d4d6d9; }
QMenu::item:disabled { color: #7d838c; background: transparent; }
QMenu::separator { height: 1px; background: #2b2d33; margin: 4px 8px; }
QGroupBox { color: #9298a0; font-weight: 600; border: 1px solid #2b2d33; border-radius: 6px; margin-top: 12px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #9298a0; }
QScrollBar:vertical { background: transparent; width: 5px; margin: 0px; }
QScrollBar::handle:vertical { background: #34373d; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #44484f; }
QScrollBar::handle:vertical:focus { background: #7ea6c9; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background: transparent; height: 5px; margin: 0px; }
QScrollBar::handle:horizontal { background: #34373d; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #44484f; }
QScrollBar::handle:horizontal:focus { background: #7ea6c9; }
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
QToolButton#groupHeader { background: transparent; border: none; border-top: 1px solid #232a35; color: #9aa3b2; font-size: 11px; font-weight: 700; padding: 8px 0px 4px; }
QToolButton#groupHeader:hover { color: #46c8f0; }
QToolButton#groupHeader:focus { color: #46c8f0; }
QFrame#stationCard { background: #10131a; border: 1px solid #262c36; border-radius: 10px; }
QToolButton#productChip { background: #0d0f13; color: #e9edf3; border: 1px solid #262c36; border-radius: 8px; padding: 7px 4px; }
QToolButton#productChip:hover { border-color: #39424f; }
QToolButton#productChip:checked { background: rgba(70, 200, 240, 14); border-color: #46c8f0; color: #46c8f0; }
QToolButton#productChip:disabled { color: #828b9b; background: #10131a; border-color: #4d5666; }
QToolButton#tiltGridButton { background: #0d0f13; color: #8b94a5; border: 1px solid #262c36; border-radius: 6px; padding: 5px 2px; }
QToolButton#tiltGridButton:hover { color: #e9edf3; border-color: #39424f; }
QToolButton#tiltGridButton:checked { background: rgba(70, 200, 240, 14); border-color: #46c8f0; color: #46c8f0; }
QLabel[layerBadge="true"] { color: #46c8f0; background: rgba(70, 200, 240, 14); border-radius: 10px; font-size: 10px; font-weight: 700; padding: 2px 9px; }
QLabel#layerSub { color: #828b9b; font-size: 10px; padding: 0px 0px 2px 24px; }
QLabel#groupCaption { color: #828b9b; font-size: 10px; }
QLabel#activeProductLabel { color: #e9edf3; font-size: 13px; font-weight: 600; padding-bottom: 4px; }
QLabel#emptyStateHint { background: rgba(20, 23, 29, 230); border: 1px solid #46c8f0; border-radius: 10px; padding: 18px 26px; color: #e9edf3; font-size: 13px; }
QLabel#offlineBanner { color: #0d0f13; background: #e0c14a; border-radius: 6px; padding: 6px 12px; font-weight: 700; }
QLabel#glmStatusNote[notice="true"] { color: #ff9a8f; font-size: 10px; }
QComboBox, QLineEdit, QDoubleSpinBox, QSpinBox, QDateTimeEdit { background: #0d0f13; color: #e9edf3; border: 1px solid #39424f; border-radius: 6px; padding: 5px 8px; selection-background-color: rgba(70, 200, 240, 90); selection-color: #0d0f13; }
QComboBox:focus, QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QDateTimeEdit:focus { border: 1px solid #46c8f0; }
QComboBox:hover, QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QDateTimeEdit:hover { border-color: #4d5666; }
QComboBox:disabled, QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QDateTimeEdit:disabled { color: #828b9b; border-color: #4d5666; background: #10131a; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::drop-down:hover { background: #232b36; }
QComboBox QLineEdit { background: transparent; border: none; padding: 0px; }
QComboBox QAbstractItemView { background: #14171d; color: #e9edf3; border: 1px solid #39424f; outline: none; selection-background-color: rgba(70, 200, 240, 70); selection-color: #e9edf3; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { background: #10131a; border-left: 1px solid #262c36; width: 18px; }
QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover, QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #232b36; }
QToolButton, QPushButton { background: #1b2028; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; padding: 6px 10px; }
QToolButton:hover, QPushButton:hover { background: #232b36; border-color: #39424f; }
QToolButton:pressed, QPushButton:pressed { background: #0d0f13; }
QToolButton:focus, QPushButton:focus { border: 1px solid #46c8f0; }
QToolButton:checked, QPushButton:checked { background: rgba(70, 200, 240, 32); border-color: #46c8f0; color: #46c8f0; }
QToolButton:disabled, QPushButton:disabled { color: #828b9b; background: #14171d; border-color: #4d5666; }
// Follow-latest toggle keeps the emphasis sizing; the checked state uses
// the default accent (no green LIVE dot).
QToolButton#followLatestButton { font-weight: 700; padding: 6px 14px; }
QCheckBox { color: #e9edf3; background: transparent; spacing: 7px; }
QCheckBox:disabled { color: #828b9b; }
QCheckBox:focus { color: #46c8f0; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #828b9b; border-radius: 3px; background: transparent; }
QCheckBox::indicator:hover { border-color: #8b94a5; }
QCheckBox::indicator:checked { border-color: #46c8f0; background: #46c8f0; }
QCheckBox::indicator:disabled { border-color: #4d5666; background: transparent; }
QCheckBox::indicator:focus { border: 2px solid #46c8f0; }
QSlider::groove:horizontal { background: #262c36; height: 4px; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #46c8f0; border-radius: 2px; }
QSlider::add-page:horizontal { background: #262c36; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0px; border-radius: 7px; background: #0d0f13; border: 2px solid #46c8f0; }
QSlider::handle:horizontal:hover { background: rgba(70, 200, 240, 40); }
QSlider::handle:horizontal:focus { background: #46c8f0; }
QSlider::groove:horizontal:disabled, QSlider::sub-page:horizontal:disabled, QSlider::add-page:horizontal:disabled { background: #4d5666; }
QSlider::handle:horizontal:disabled { border: 2px solid #4d5666; background: #10131a; }
QProgressBar { background: #1c212a; border: none; border-radius: 2px; height: 5px; text-align: center; }
QProgressBar::chunk { background: #46c8f0; border-radius: 2px; }
QStatusBar { background: #10131a; color: #8b94a5; border-top: 1px solid #232a35; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: #8b94a5; background: transparent; }
QStatusBar QToolButton { padding: 3px 10px; }
QToolTip { background: #14171d; color: #e9edf3; border: 1px solid #2b3340; border-radius: 4px; padding: 5px 8px; }
QTextEdit#shortcutList { background: #0d0f13; color: #e9edf3; border: 1px solid #39424f; border-radius: 6px; selection-background-color: rgba(70, 200, 240, 90); }
QTabWidget#sidebarTabs::pane { border: none; background: transparent; }
QTabBar::tab { background: #10131a; color: #8b94a5; border: 1px solid #262c36; border-bottom: none; border-top-left-radius: 6px; border-top-right-radius: 6px; padding: 7px 4px; font-size: 11px; font-weight: 700; }
QTabBar::tab:selected { background: #1a2029; color: #46c8f0; border-color: #39424f; }
QTabBar::tab:hover:!selected { color: #e9edf3; }
QTabBar::tab:focus { border: 1px solid #46c8f0; }
QTabBar#productTabBar { background: #0d0f13; }
QTabBar#productTabBar::tab { padding: 7px 12px; }
QTabBar#productTabBar::tab:disabled { color: #828b9b; background: #10131a; }
QListWidget#warningList { background: #0d0f13; color: #e9edf3; border: 1px solid #39424f; border-radius: 6px; outline: none; font-size: 11px; }
QListWidget#warningList::item { padding: 6px 8px; border-bottom: 1px solid #1c212a; }
QListWidget#warningList::item:hover { background: rgba(70, 200, 240, 18); }
QListWidget#warningList::item:selected { background: rgba(70, 200, 240, 45); color: #e9edf3; }
QListWidget#warningList:focus { border: 1px solid #46c8f0; }
QListWidget#commandList { background: #10131a; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; outline: none; }
QListWidget#commandList::item { border-radius: 4px; padding: 6px 10px; }
QListWidget#commandList::item:selected { background: rgba(70, 200, 240, 45); color: #e9edf3; }
QMenuBar { background: #14171d; color: #e9edf3; border: none; }
QMenuBar::item { background: transparent; padding: 5px 11px; border-radius: 5px; }
QMenuBar::item:selected { background: rgba(70, 200, 240, 26); }
QMenuBar::item:pressed { background: rgba(70, 200, 240, 42); }
QMenuBar::item:focus { background: rgba(70, 200, 240, 26); color: #46c8f0; }
QMenu { background: #14171d; color: #e9edf3; border: 1px solid #262c36; border-radius: 6px; padding: 4px; }
QMenu::item { border-radius: 4px; padding: 5px 22px 5px 12px; }
QMenu::item:selected { background: rgba(70, 200, 240, 70); }
QMenu::item:disabled { color: #828b9b; background: transparent; }
QMenu::separator { height: 1px; background: #232a35; margin: 4px 8px; }
QGroupBox { color: #c7cfdb; font-weight: 700; border: 1px solid #232a35; border-radius: 8px; margin-top: 14px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #9aa3b2; }
QScrollBar:vertical { background: transparent; width: 7px; margin: 0px; }
QScrollBar::handle:vertical { background: #2b3340; min-height: 28px; border-radius: 3px; }
QScrollBar::handle:vertical:hover { background: #3d4757; }
QScrollBar::handle:vertical:focus { background: #46c8f0; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background: transparent; height: 7px; margin: 0px; }
QScrollBar::handle:horizontal { background: #2b3340; min-width: 28px; border-radius: 3px; }
QScrollBar::handle:horizontal:hover { background: #3d4757; }
QScrollBar::handle:horizontal:focus { background: #46c8f0; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; border: none; background: none; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
)QSS");
}

} // namespace

QString appStylesheet(const AppTheme theme, const double fontScale) {
    const auto base =
        theme == AppTheme::Prototype ? prototypeStylesheet() : classicStylesheet();
    // Font-scale chrome: the app font (applyFontScale) reaches every widget
    // using the default font, but the pinned pixel sizes above (dock
    // titles, badges, tabs, captions) outrank it. Late override rules win
    // ties in Qt, so these scale the chrome without touching either theme.
    const auto clamped = std::clamp(fontScale, 0.8, 1.5);
    const int small = qRound(10.0 * clamped);  // dock titles, badges, tabs, captions
    const int medium = qRound(11.0 * clamped);  // section labels, warning list
    const int large = qRound(12.0 * clamped);  // active product label
    return base +
           QStringLiteral(
               "QDockWidget::title{font-size:%1px}"
               "QLabel#section{font-size:%2px}"
               "QToolButton#groupHeader{font-size:%1px}"
               "QLabel[layerBadge=\"true\"]{font-size:%1px}"
               "QLabel#layerSub{font-size:%1px}"
               "QLabel#groupCaption{font-size:%1px}"
               "QLabel#activeProductLabel{font-size:%3px}"
               "QTabBar::tab{font-size:%1px}"
               "QListWidget#warningList{font-size:%2px}")
               .arg(small)
               .arg(medium)
               .arg(large);
}

} // namespace cappedweather
