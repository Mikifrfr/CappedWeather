// Hermetic checks for cappedweather_ui_logic: the Qt-Core+Gui-only library that
// hosts QString/QDateTime/QFont policy extracted from src/app/main_window.cpp.
//
// Deliberately not part of tests/ui_smoke_test.cpp (frozen for the extraction
// series), and deliberately built on QGuiApplication: a module added to the
// library that needs QApplication or QWidget stops compiling here instead of
// quietly turning the extraction target into a second UI suite.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/Release/cappedweather_ui_logic_tests

#include "app/ui_logic/anchor.hpp"

#include <QGuiApplication>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) {
        std::printf("PASS %s\n", what);
        return;
    }
    ++failures;
    std::printf("FAIL %s\n", what);
}

// S0 acceptance: the library links and its seed symbol resolves. An empty or
// missing anchor means the static library was not linked into this executable
// (or the symbol was renamed), i.e. the target is not yet usable as the home
// for extracted policy.
void testUiLogicTargetLinks()
{
    const char* anchor = cappedweatherUiLogicAnchor();
    check(anchor != nullptr, "anchor pointer is non-null");
    if (anchor == nullptr) {
        return;
    }
    check(std::strlen(anchor) > 0, "anchor string is non-empty");
    check(std::strncmp(anchor, "cappedweather-ui-logic/",
                       sizeof("cappedweather-ui-logic/") - 1) == 0,
          "anchor names the library");
    std::printf("      anchor=%s\n", anchor);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    std::printf("cappedweather_ui_logic_tests: platform=%s\n",
                QGuiApplication::platformName().toUtf8().constData());

    testUiLogicTargetLinks();

    if (failures != 0) {
        std::printf("ui logic tests failed: %d check(s)\n", failures);
        return 1;
    }
    std::printf("ui logic tests passed\n");
    return 0;
}
