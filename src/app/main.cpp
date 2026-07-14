#include "app/main_window.hpp"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("CappedWeather"));
    application.setOrganizationName(QStringLiteral("CappedWeather"));
#ifdef CAPPEDWEATHER_VERSION
    application.setApplicationVersion(QStringLiteral(CAPPEDWEATHER_VERSION));
#endif

    cappedweather::MainWindow window;
    window.show();
    return application.exec();
}
