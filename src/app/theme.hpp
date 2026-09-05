#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace cappedweather {

// Switchable application identity. Classic is the original dark console;
// Prototype ("Squall") is the refined-dark-ops redesign preview. Both cover
// the full widget set so flipping themes never leaves controls unstyled.
enum class AppTheme {
    Classic = 0,
    Prototype = 1,
};

QString appThemeName(AppTheme theme);
AppTheme appThemeFromInt(int value);
QPalette appPalette(AppTheme theme);
QString appStylesheet(AppTheme theme);
// Accent used for canvas-painted chrome the stylesheet cannot reach
// (active-panel border). Applied to every panel by MainWindow::applyTheme.
QColor appAccent(AppTheme theme);

} // namespace cappedweather
