#include "pngencoder.h"

#include <QImage>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#define ZLIB_CONST // const next_in
#include <zlib.h>

namespace blink {
namespace {

// A strip is at least this big, so that it pays for its deflate state and probe. There are a few strips per thread:
// threads take the next free strip when done, which balances strips of photos (slow) against strips of text (fast).
constexpr int kMinStripRows = 32;
constexpr qsizetype kMinStripPixels = 64 * 1024;
constexpr int kStripsPerThread = 4;

// zlib-ng level 6 with the probe below gives the smallest files of the combinations measured on 4K screenshots and
// stays within ~15 ms; levels 7-9 cost 2-10x the time for a few percent.
constexpr int kLevel = 6;

// Text and pixel art compress best unfiltered (their rows repeat exactly), photos best with libpng's adaptive
// filtering. The first rows of every strip go through both, and the smaller output does the rest of the strip.
constexpr int kProbeRows = 8;

// Filter loops work in chunks with a constant trip count, which compilers vectorize at -O2.
constexpr size_t kChunk = 32;

constexpr size_t kChunkHeader = 8; // length and type of a PNG chunk
constexpr size_t kZlibHeader = 2;
constexpr std::array<uchar, 4> kIhdrType{'I', 'H', 'D', 'R'};
constexpr std::array<uchar, 4> kIdatType{'I', 'D', 'A', 'T'};
constexpr std::array<uchar, 4> kIendType{'I', 'E', 'N', 'D'};

// Byte offset of alpha within a pixel of the formats with a fast path.
constexpr size_t kArgbAlphaByte = std::endian::native == std::endian::little ? 3 : 0;
constexpr size_t kRgbaAlphaByte = 3;

enum class Layout {
    Argb, // native-endian 0xAARRGGBB words: RGB32, ARGB32, ARGB32_Premultiplied
    Rgba, // R, G, B, A bytes: RGBX8888, RGBA8888, RGBA8888_Premultiplied
    Rgb,  // R, G, B bytes: RGB888
};

// Same rounding as qUnpremultiply().
constexpr std::array<uint32_t, 256> kInverseAlpha = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t a = 1; a < 256; ++a)
        table[a] = 0x00ff00ffU / a;
    return table;
}();

uchar unpremultiply(uint32_t channel, uint32_t alpha)
{
    return static_cast<uchar>((channel * kInverseAlpha[alpha] + 0x8000U) >> 16);
}

uint32_t load32(const uchar *p)
{
    uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

uchar *put32(uchar *p, uint32_t value)
{
    p[0] = static_cast<uchar>(value >> 24);
    p[1] = static_cast<uchar>(value >> 16);
    p[2] = static_cast<uchar>(value >> 8);
    p[3] = static_cast<uchar>(value);
    return p + 4;
}

// Writes a row as 8-bit RGB, or straight RGBA when `alpha`.
void convertRow(const uchar *src, uchar *dst, int width, Layout layout, bool premultiplied, bool alpha)
{
    switch (layout) {
    case Layout::Argb:
        if (!alpha) {
            for (int x = 0; x < width; ++x, src += 4, dst += 3) {
                const uint32_t p = load32(src);
                dst[0] = static_cast<uchar>(p >> 16);
                dst[1] = static_cast<uchar>(p >> 8);
                dst[2] = static_cast<uchar>(p);
            }
            return;
        }
        for (int x = 0; x < width; ++x, src += 4, dst += 4) {
            const uint32_t p = load32(src);
            const uint32_t a = p >> 24;
            uint32_t r = (p >> 16) & 0xff;
            uint32_t g = (p >> 8) & 0xff;
            uint32_t b = p & 0xff;
            if (premultiplied && a != 255) {
                r = unpremultiply(r, a);
                g = unpremultiply(g, a);
                b = unpremultiply(b, a);
            }
            dst[0] = static_cast<uchar>(r);
            dst[1] = static_cast<uchar>(g);
            dst[2] = static_cast<uchar>(b);
            dst[3] = static_cast<uchar>(a);
        }
        return;
    case Layout::Rgba:
        if (!alpha) {
            for (int x = 0; x < width; ++x, src += 4, dst += 3)
                std::memcpy(dst, src, 3);
        } else if (!premultiplied) {
            std::memcpy(dst, src, static_cast<size_t>(width) * 4);
        } else {
            for (int x = 0; x < width; ++x, src += 4, dst += 4) {
                const uint32_t a = src[3];
                if (a == 255) {
                    std::memcpy(dst, src, 4);
                    continue;
                }
                dst[0] = unpremultiply(src[0], a);
                dst[1] = unpremultiply(src[1], a);
                dst[2] = unpremultiply(src[2], a);
                dst[3] = static_cast<uchar>(a);
            }
        }
        return;
    case Layout::Rgb:
        std::memcpy(dst, src, static_cast<size_t>(width) * 3);
        return;
    }
}

int filterCost(uchar filtered)
{
    return std::abs(static_cast<int>(static_cast<int8_t>(filtered)));
}

// Filters bytes [begin, end) of `cur` with Sub, Up, Average and Paeth and adds each filter's sum of absolute values,
// None's included, to `sums`. `cur` and `prev` are preceded by Bpp zero bytes.
template<size_t Bpp>
void filterSpan(const uchar *__restrict cur, const uchar *__restrict prev, size_t begin, size_t end, uchar *__restrict sub,
                uchar *__restrict up, uchar *__restrict average, uchar *__restrict paeth, uint32_t *__restrict sums)
{
    uint32_t none = 0, subSum = 0, upSum = 0, averageSum = 0, paethSum = 0;
    for (size_t i = begin; i < end; ++i) {
        // 16 bits hold every intermediate (|p + q| <= 510) and vectorize twice as wide as int.
        const int16_t x = cur[i], a = cur[i - Bpp], b = prev[i], c = prev[i - Bpp];
        const auto p = static_cast<int16_t>(b - c);
        const auto q = static_cast<int16_t>(a - c);
        const auto pa = static_cast<int16_t>(std::abs(p));
        const auto pb = static_cast<int16_t>(std::abs(q));
        const auto pc = static_cast<int16_t>(std::abs(p + q));
        const int16_t predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        sub[i] = static_cast<uchar>(x - a);
        up[i] = static_cast<uchar>(x - b);
        average[i] = static_cast<uchar>(x - ((a + b) >> 1));
        paeth[i] = static_cast<uchar>(x - predictor);
        none += filterCost(static_cast<uchar>(x));
        subSum += filterCost(sub[i]);
        upSum += filterCost(up[i]);
        averageSum += filterCost(average[i]);
        paethSum += filterCost(paeth[i]);
    }
    sums[0] += none;
    sums[1] += subSum;
    sums[2] += upSum;
    sums[3] += averageSum;
    sums[4] += paethSum;
}

// libpng's heuristic: the filter whose output has the minimum sum of absolute values. `cur - 1` is a zero byte, so
// cur - 1 is the row with filter None; filtered[1..4] have room for the other filters' rows. Returns the chosen row,
// filter type byte first.
template<size_t Bpp>
const uchar *filterRow(const uchar *cur, const uchar *prev, size_t size, uchar *const *filtered)
{
    std::array<uint32_t, 5> sums{};
    const size_t chunked = size / kChunk * kChunk;
    size_t i = 0;
    for (; i < chunked; i += kChunk)
        filterSpan<Bpp>(cur, prev, i, i + kChunk, filtered[1] + 1, filtered[2] + 1, filtered[3] + 1, filtered[4] + 1, sums.data());
    filterSpan<Bpp>(cur, prev, i, size, filtered[1] + 1, filtered[2] + 1, filtered[3] + 1, filtered[4] + 1, sums.data());
    const auto best = static_cast<size_t>(std::ranges::min_element(sums) - sums.begin());
    if (best == 0)
        return cur - 1;
    filtered[best][0] = static_cast<uchar>(best);
    return filtered[best];
}

// Raw deflate into a buffer that starts with `header` bytes left for the caller.
class Deflater {
public:
    Deflater(size_t header, size_t input)
    {
        ok_ = deflateInit2(&stream_, kLevel, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        if (!ok_)
            return;
        // The bound holds for Z_FINISH; other flushes may add a few bytes, and grow() covers the rest.
        capacity_ = header + deflateBound(&stream_, static_cast<uLong>(input)) + 64;
        buffer_ = std::make_unique_for_overwrite<uchar[]>(capacity_);
        stream_.next_out = buffer_.get() + header;
        stream_.avail_out = static_cast<uInt>(capacity_ - header);
    }
    ~Deflater() { deflateEnd(&stream_); }
    Deflater(const Deflater &) = delete;
    Deflater &operator=(const Deflater &) = delete;

    void write(const uchar *data, size_t size)
    {
        adler_ = adler32_z(adler_, data, size);
        stream_.next_in = data;
        stream_.avail_in = static_cast<uInt>(size);
        run(Z_NO_FLUSH);
    }
    void flush(int mode) { run(mode); }

    bool ok() const { return ok_; }
    size_t size() const { return static_cast<size_t>(stream_.next_out - buffer_.get()); }
    uLong adler() const { return adler_; }
    uchar *data() const { return buffer_.get(); }
    std::unique_ptr<uchar[]> take() { return std::move(buffer_); }

private:
    void run(int flush)
    {
        while (ok_) {
            if (stream_.avail_out == 0)
                grow();
            deflate(&stream_, flush);
            if (stream_.avail_out != 0 && stream_.avail_in == 0)
                return;
        }
    }
    void grow()
    {
        const size_t used = capacity_;
        capacity_ *= 2;
        auto buffer = std::make_unique_for_overwrite<uchar[]>(capacity_);
        std::memcpy(buffer.get(), buffer_.get(), used);
        buffer_ = std::move(buffer);
        stream_.next_out = buffer_.get() + used;
        stream_.avail_out = static_cast<uInt>(capacity_ - used);
    }

    z_stream stream_{};
    std::unique_ptr<uchar[]> buffer_;
    size_t capacity_ = 0;
    uLong adler_ = adler32(0, nullptr, 0);
    bool ok_ = false;
};

struct Strip {
    int first = 0; // rows
    int end = 0;
    QImage converted; // formats without a fast path: rows max(first - 1, 0) to end - 1 as RGBA8888 or RGB888
    // An IDAT chunk without its CRC: length, type and data (the zlib header first in the first strip). The last
    // strip's data still lacks the stream's Adler-32, which needs all strips.
    std::unique_ptr<uchar[]> bytes;
    size_t size = 0;
    uLong crc = 0;   // of the chunk type and data so far
    uLong adler = 0; // of the uncompressed data
    size_t rawSize = 0;
    bool ok = false;
};

class Encoder {
public:
    Encoder(const QImage &image, int strips, int threads);

    // Called once per thread index, by `threads` threads all running at once.
    void run(int thread);
    QByteArray assemble() const;

private:
    const uchar *row(const Strip &strip, int y) const
    {
        return convert_ ? strip.converted.constScanLine(y - std::max(strip.first - 1, 0)) : image_.constScanLine(y);
    }
    void convert(Strip &strip) const;
    bool opaque(const Strip &strip) const;
    void encode(Strip &strip, bool last) const;

    const QImage &image_;
    const int width_;
    const int height_;
    Layout layout_ = Layout::Argb;
    bool convert_ = false;
    bool alpha_;
    bool premultiplied_;
    std::vector<Strip> strips_;
    const int threads_;
    std::barrier<> opacityChecked_;
    std::atomic<bool> translucent_ = false;
    std::atomic<size_t> nextStrip_ = 0;
};

Encoder::Encoder(const QImage &image, int strips, int threads)
    : image_(image)
    , width_(image.width())
    , height_(image.height())
    , alpha_(image.hasAlphaChannel())
    , premultiplied_(image.format() == QImage::Format_ARGB32_Premultiplied || image.format() == QImage::Format_RGBA8888_Premultiplied)
    , strips_(static_cast<size_t>(strips))
    , threads_(threads)
    , opacityChecked_(threads)
{
    switch (image.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        layout_ = Layout::Argb;
        break;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        layout_ = Layout::Rgba;
        break;
    case QImage::Format_RGB888:
        layout_ = Layout::Rgb;
        break;
    default:
        convert_ = true;
        layout_ = alpha_ ? Layout::Rgba : Layout::Rgb;
        break;
    }
    for (int i = 0; i < strips; ++i) {
        strips_[static_cast<size_t>(i)].first = static_cast<int>(static_cast<qsizetype>(height_) * i / strips);
        strips_[static_cast<size_t>(i)].end = static_cast<int>(static_cast<qsizetype>(height_) * (i + 1) / strips);
    }
}

void Encoder::convert(Strip &strip) const
{
    const int first = std::max(strip.first - 1, 0);
    QImage view(image_.constScanLine(first), width_, strip.end - first, image_.bytesPerLine(), image_.format());
    view.setColorTable(image_.colorTable());
    strip.converted = view.convertToFormat(alpha_ ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
}

bool Encoder::opaque(const Strip &strip) const
{
    const size_t alphaByte = layout_ == Layout::Argb ? kArgbAlphaByte : kRgbaAlphaByte;
    for (int y = strip.first; y < strip.end; ++y) {
        if (translucent_.load(std::memory_order_relaxed))
            return false;
        const uchar *p = row(strip, y);
        uint32_t all = ~0U;
        for (int x = 0; x < width_; ++x)
            all &= load32(p + (static_cast<size_t>(x) * 4));
        std::array<uchar, 4> bytes;
        std::memcpy(bytes.data(), &all, 4);
        if (bytes[alphaByte] != 255)
            return false;
    }
    return true;
}

void Encoder::run(int thread)
{
    if (alpha_) {
        // Every strip needs the colour type, so opacity is settled first, each thread checking a fixed share.
        const size_t count = strips_.size();
        const auto threads = static_cast<size_t>(threads_);
        for (size_t i = count * static_cast<size_t>(thread) / threads; i < count * static_cast<size_t>(thread + 1) / threads; ++i) {
            if (convert_)
                convert(strips_[i]);
            if (!opaque(strips_[i]))
                translucent_.store(true, std::memory_order_relaxed);
        }
        opacityChecked_.arrive_and_wait();
    }
    for (size_t i = nextStrip_.fetch_add(1, std::memory_order_relaxed); i < strips_.size();
         i = nextStrip_.fetch_add(1, std::memory_order_relaxed)) {
        if (convert_ && !alpha_)
            convert(strips_[i]);
        encode(strips_[i], i + 1 == strips_.size());
    }
}

void Encoder::encode(Strip &strip, bool last) const
{
    const bool alpha = translucent_.load(std::memory_order_relaxed);
    const size_t bpp = alpha ? 4 : 3;
    const size_t rowBytes = static_cast<size_t>(width_) * bpp;
    strip.rawSize = static_cast<size_t>(strip.end - strip.first) * (rowBytes + 1);

    // Two rows preceded by bpp zero bytes (the left neighbours at x = 0, and filter type None right before the
    // row), and the Sub, Up, Average and Paeth rows.
    const size_t padded = bpp + rowBytes;
    auto buffers = std::make_unique<uchar[]>(2 * padded + 4 * (1 + rowBytes));
    uchar *prev = buffers.get() + bpp;
    uchar *cur = prev + padded;
    std::array<uchar *, 5> filtered{};
    for (size_t f = 1; f < 5; ++f)
        filtered[f] = buffers.get() + 2 * padded + (f - 1) * (1 + rowBytes);
    if (strip.first > 0)
        convertRow(row(strip, strip.first - 1), prev, width_, layout_, premultiplied_, alpha);
    const auto filter = [&] {
        return bpp == 3 ? filterRow<3>(cur, prev, rowBytes, filtered.data()) : filterRow<4>(cur, prev, rowBytes, filtered.data());
    };

    const size_t header = kChunkHeader + (strip.first == 0 ? kZlibHeader : 0);
    std::optional<Deflater> unfiltered(std::in_place, header, strip.rawSize);
    std::optional<Deflater> adaptive(std::in_place, header, strip.rawSize);
    const int probeEnd = std::min(strip.first + kProbeRows, strip.end);
    int y = strip.first;
    for (; y < probeEnd; ++y, std::swap(prev, cur)) {
        convertRow(row(strip, y), cur, width_, layout_, premultiplied_, alpha);
        unfiltered->write(cur - 1, rowBytes + 1);
        adaptive->write(filter(), rowBytes + 1);
    }
    unfiltered->flush(Z_BLOCK);
    adaptive->flush(Z_BLOCK);
    const bool useUnfiltered = unfiltered->size() <= adaptive->size();
    (useUnfiltered ? adaptive : unfiltered).reset();
    Deflater &deflater = useUnfiltered ? *unfiltered : *adaptive;
    for (; y < strip.end; ++y, std::swap(prev, cur)) {
        convertRow(row(strip, y), cur, width_, layout_, premultiplied_, alpha);
        deflater.write(useUnfiltered ? cur - 1 : filter(), rowBytes + 1);
    }
    deflater.flush(last ? Z_FINISH : Z_FULL_FLUSH);
    if (!deflater.ok())
        return;

    uchar *bytes = deflater.data();
    put32(bytes, static_cast<uint32_t>(deflater.size() - kChunkHeader + (last ? 4 : 0)));
    std::ranges::copy(kIdatType, bytes + 4);
    if (strip.first == 0) {
        bytes[kChunkHeader] = 0x78; // deflate with a 32 KiB window
        bytes[kChunkHeader + 1] = 0x9c; // default level, and a header check that makes the pair a multiple of 31
    }
    strip.size = deflater.size();
    strip.crc = crc32_z(0, bytes + 4, strip.size - 4);
    strip.adler = deflater.adler();
    strip.bytes = deflater.take();
    strip.ok = true;
}

QByteArray Encoder::assemble() const
{
    static constexpr uchar kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    constexpr size_t kIhdr = kChunkHeader + 13 + 4;
    constexpr size_t kIend = kChunkHeader + 4;
    size_t total = sizeof(kSignature) + kIhdr + kIend + 4; // the last 4: Adler-32
    for (const Strip &strip : strips_) {
        if (!strip.ok)
            return {};
        total += strip.size + 4;
    }

    QByteArray png(static_cast<qsizetype>(total), Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(png.data());
    std::memcpy(out, kSignature, sizeof(kSignature));
    out += sizeof(kSignature);

    put32(out, 13);
    std::ranges::copy(kIhdrType, out + 4);
    put32(out + 8, static_cast<uint32_t>(width_));
    put32(out + 12, static_cast<uint32_t>(height_));
    out[16] = 8;                                                             // bits per channel
    out[17] = translucent_.load(std::memory_order_relaxed) ? 6 : 2;          // RGBA : RGB
    out[18] = out[19] = out[20] = 0;                                         // deflate, adaptive filtering, no interlace
    put32(out + 21, static_cast<uint32_t>(crc32_z(0, out + 4, 17)));
    out += kIhdr;

    uLong adler = adler32(0, nullptr, 0);
    for (const Strip &strip : strips_) {
        std::memcpy(out, strip.bytes.get(), strip.size);
        out += strip.size;
        adler = adler32_combine(adler, strip.adler, z_off_t(strip.rawSize));
        uLong crc = strip.crc;
        if (&strip == &strips_.back()) {
            put32(out, static_cast<uint32_t>(adler));
            crc = crc32_z(crc, out, 4);
            out += 4;
        }
        out = put32(out, static_cast<uint32_t>(crc));
    }

    put32(out, 0);
    std::ranges::copy(kIendType, out + 4);
    put32(out + 8, static_cast<uint32_t>(crc32_z(0, out + 4, 4)));
    return png;
}

} // namespace

QByteArray encodePng(const QImage &image)
{
    if (image.isNull())
        return {};
    const qsizetype height = image.height();
    const qsizetype cores = std::max(1U, std::thread::hardware_concurrency());
    const auto strips = static_cast<int>(std::clamp(std::min(height / kMinStripRows, height * image.width() / kMinStripPixels),
                                                    static_cast<qsizetype>(1), cores * kStripsPerThread));
    const auto threads = static_cast<int>(std::min<qsizetype>(cores, strips));
    Encoder encoder(image, strips, threads);
    {
        // Own threads rather than the global pool: callers may be pool threads themselves.
        std::vector<std::jthread> workers;
        workers.reserve(static_cast<size_t>(threads - 1));
        for (int i = 1; i < threads; ++i)
            workers.emplace_back([&encoder, i] { encoder.run(i); });
        encoder.run(0);
    }
    return encoder.assemble();
}

} // namespace blink
