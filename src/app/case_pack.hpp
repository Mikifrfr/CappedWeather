#pragma once

// Case-pack export/import: a shared .cwcase zip (deflate via zlib) holding
// spilled volumes plus a metadata manifest, so a shared case reproduces
// exactly. Layout inside the zip:
//
//   manifest.json            caseFormat 1: radar, frames, panels, placefiles
//   volumes/<safe>.cwdv      one spilled volume per frame (volume_spill format)
//   placefiles/<n>.txt       bundled local placefile bytes (remote URLs stay
//                            URLs in the manifest and re-fetch on import)
//
// Everything malformed fails closed: bad JSON, missing volume files,
// directory traversal in entry names, CRC/size mismatch, or absurd sizes
// abort with an error and no partial session. Without zlib at build time
// every entry point reports "needs a zlib build" and writes nothing.

#include "app/session_store.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace cappedweather {

inline constexpr int kCasePackFormat = 1;
// Zip-slip + bomb guards: packs are far smaller in practice (volumes are
// tens of MB each); anything beyond these bounds is rejected, not read.
inline constexpr int kCasePackMaxEntries = 256;
inline constexpr qint64 kCasePackMaxUncompressedBytes = 2LL * 1024 * 1024 * 1024;
// One entry never legitimately approaches this (spilled volumes are tens of
// MB); without it a single lying central-directory size passes the total
// cap and then crashes on a 2 GiB contiguous resize.
inline constexpr quint32 kCasePackMaxEntryBytes = 256U * 1024U * 1024U;

struct CasePanelState {
    QString radarId;
    int product = 0;  // ProductKind as int
    int tilt = -1;  // elevationNumber, -1 = all tilts
};

struct CaseFrame {
    QString volumeId;
    QDateTime dataTime;
    int chunkCount = 0;
    int failedChunks = 0;
    QString file;  // zip-relative path under volumes/
};

struct CaseManifest {
    QString radarId;
    int frameIndex = -1;
    QVector<CaseFrame> frames;
    QVector<CasePanelState> panels;
    QStringList placefileUrls;  // remote feeds, re-fetched on import
    QStringList placefileFiles;  // zip-relative paths under placefiles/
    QDateTime exportedAt;
};

// Parses and validates a manifest document. Unknown fields are ignored;
// missing required fields, a wrong format version, frames without volume
// ids or files, or an out-of-range frameIndex fail closed to nullopt.
std::optional<CaseManifest> parseCaseManifest(const QByteArray& data, QString* error);
QByteArray writeCaseManifest(const CaseManifest& manifest);

// Minimal zip (stored + deflated entries, UTF-8 names, CRC32). Enough for
// case-packs; not a general archiver.
bool zipAvailable();
bool zipPackFiles(const QVector<QPair<QString, QString>>& localFiles,  // (zipName, diskPath)
                  const QString& zipPath, QString* error);
// Extracts all entries under destDir, returns zip-relative names. Rejects
// absolute paths and ".." traversal, caps entry count and total
// uncompressed size. On any failure nothing is trusted: already-extracted
// files are removed and an error is returned.
QStringList zipExtractFiles(const QString& zipPath, const QString& destDir, QString* error);

// Progress callback for long packs: called per volume, return false to abort.
using CasePackProgress = std::function<bool(int done, int total)>;

// Stages every manifest frame through the spill path (copy plus decode
// check) and installs the rebuilt history on the session. The result
// mirrors a live session after its frames spill: empty chunks with spilled
// paths, so scrubbing faults back in through sessionFrameChunk. Fails
// closed with no partial session: a bad volume removes what landed and
// leaves the session untouched.
bool populateImportedSession(RadarSession& session, const CaseManifest& manifest,
                             const QString& unpackedDir, QString* error);

}  // namespace cappedweather
