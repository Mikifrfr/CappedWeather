#pragma once

// Version marker for the Qt-Core+Gui-only UI policy library.
//
// cappedweather_ui_logic exists so that QString/QDateTime/QFont policy
// extracted from src/app/main_window.cpp can be proven hermetically, in a
// target that pulls neither QWidget nor the frozen tests/ui_smoke_test.cpp.
// It links Qt6::Core and Qt6::Gui and nothing else: a module that needs
// QWidget belongs in cappedweather_ui, not here.
//
// The anchor is the trivial symbol that proves the target compiles, links and
// runs headless before the first real module lands. Keep it: it costs one
// function and it is the S0 acceptance check.
//
// Global scope is deliberate -- this is the card's literal signature and a
// C-style version marker. Modules extracted here later follow the repository's
// `namespace cappedweather` convention.
const char* cappedweatherUiLogicAnchor();
