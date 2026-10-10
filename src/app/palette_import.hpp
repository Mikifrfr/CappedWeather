#pragma once

// GRLevelX color tables (.pal) as custom reflectivity stops: the Gibson
// Ridge text format (`Product:`/`Units:`/`Step:`/`Decimals:`/`Scale:`
// headers plus `Color:`/`Color4:`/`SolidColor:`/`SolidColor4:` rules,
// `;` comments), validated live against redteamwx RTWX_REF.pal 2026-10-10.
// Mapping onto the app step palette: each rule's start color becomes a
// stop at its value (sorted ascending); `SolidColor` maps exactly, plain
// `Color` blends approximate the GR interpolation as steps, sharp-break
// RGB2 end colors collapse to the next stop, and Color4 alpha drops (stops
// carry no per-stop alpha; opacity still applies). Pure, hermetic,
// fail-closed with a line-numbered error.

#include "app/radar_map_canvas.hpp"

#include <QString>
#include <QVector>

#include <optional>

namespace cappedweather {

struct GrColorTable {
    QString product;
    QString units;
    QVector<RadarMapCanvas::PaletteStop> stops;
};

// True when the text carries GR color-table statements (any header or
// color rule); the importer uses this to pick the GR path over CSV.
bool looksLikeGrColorTable(const QString& text);
// Parses a GR color table; nullopt with a line-numbered error on any
// malformed statement, unknown statement, or fewer than two stops.
std::optional<GrColorTable> parseGrColorTable(const QString& text, QString* error = nullptr);

}  // namespace cappedweather
