#include "app/ui_logic/anchor.hpp"

namespace {

// Diagnostic string, not a semantic version: bump the trailing number only when
// the library's contract changes in a way a caller must notice.
constexpr char kUiLogicAnchor[] = "cappedweather-ui-logic/1";

} // namespace

const char* cappedweatherUiLogicAnchor()
{
    return kUiLogicAnchor;
}
