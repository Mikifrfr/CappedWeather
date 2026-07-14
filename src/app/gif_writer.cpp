#include "app/gif_writer.hpp"

#include <QFile>

#include <algorithm>
#include <cstdint>

namespace cappedweather {
namespace {

void append16(QByteArray& output, const int value) {
    output.append(static_cast<char>(value & 0xff));
    output.append(static_cast<char>((value >> 8) & 0xff));
}

QByteArray literalLzw(const QImage& image) {
    QByteArray packed;
    std::uint32_t bits = 0;
    int bitCount = 0;
    auto code = [&](const int value) {
        bits |= static_cast<std::uint32_t>(value) << bitCount;
        bitCount += 9;
        while (bitCount >= 8) {
            packed.append(static_cast<char>(bits & 0xff));
            bits >>= 8;
            bitCount -= 8;
        }
    };
    constexpr int clearCode = 256;
    constexpr int endCode = 257;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = line[x];
            const auto index = ((qRed(pixel) >> 5) << 5) |
                               ((qGreen(pixel) >> 5) << 2) | (qBlue(pixel) >> 6);
            code(clearCode);
            code(index);
        }
    }
    code(endCode);
    if (bitCount > 0) packed.append(static_cast<char>(bits & 0xff));
    return packed;
}

void appendSubBlocks(QByteArray& output, const QByteArray& data) {
    for (qsizetype offset = 0; offset < data.size();) {
        const auto count = std::min<qsizetype>(255, data.size() - offset);
        output.append(static_cast<char>(count));
        output.append(data.constData() + offset, count);
        offset += count;
    }
    output.append('\0');
}

} // namespace

bool writeAnimatedGif(const QString& path, const QVector<QImage>& frames,
                      const int delayCentiseconds, QString* error) {
    if (frames.isEmpty() || frames.first().isNull()) {
        if (error) *error = QStringLiteral("No animation frames were supplied");
        return false;
    }
    const auto width = frames.first().width();
    const auto height = frames.first().height();
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        if (error) *error = QStringLiteral("GIF dimensions are outside the supported range");
        return false;
    }

    QByteArray output("GIF89a", 6);
    append16(output, width);
    append16(output, height);
    output.append(static_cast<char>(0xf7)); // 256-color global table, 8-bit color resolution.
    output.append('\0');
    output.append('\0');
    for (int index = 0; index < 256; ++index) {
        output.append(static_cast<char>(((index >> 5) & 7) * 255 / 7));
        output.append(static_cast<char>(((index >> 2) & 7) * 255 / 7));
        output.append(static_cast<char>((index & 3) * 255 / 3));
    }
    output.append("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);

    for (const auto& original : frames) {
        auto frame = original;
        if (frame.size() != QSize(width, height))
            frame = frame.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        frame = frame.convertToFormat(QImage::Format_RGB32);
        output.append("\x21\xf9\x04\x04", 4);
        append16(output, std::clamp(delayCentiseconds, 1, 65535));
        output.append('\0');
        output.append('\0');
        output.append('\x2c');
        append16(output, 0);
        append16(output, 0);
        append16(output, width);
        append16(output, height);
        output.append('\0');
        output.append('\x08');
        appendSubBlocks(output, literalLzw(frame));
    }
    output.append('\x3b');

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size()) {
        if (error) *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace cappedweather
