#include "softwarevideoconverter.h"
#include "../../app/streaming/video/ffmpeg-testframes.h"

#include <libplacebo/vulkan.h>
extern "C" {
#include <libavutil/pixdesc.h>
}
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
struct Device {
    pl_vulkan vk = nullptr;
    ~Device() { pl_vulkan_destroy(&vk); }
};
struct Frame {
    AVFrame* value = av_frame_alloc();
    ~Frame() { av_frame_free(&value); }
};
struct Context {
    AVCodecContext* value;
    explicit Context(AVCodecID codec) : value(avcodec_alloc_context3(avcodec_find_decoder(codec))) {}
    ~Context() { avcodec_free_context(&value); }
};
struct Texture {
    pl_gpu gpu;
    pl_tex value;
    ~Texture() { pl_tex_destroy(gpu, &value); }
};
int getBuffer(AVCodecContext* context, AVFrame* frame, int flags)
{
    return static_cast<PlVkFramePool*>(context->opaque)->getBuffer(context, frame, flags);
}

std::vector<uint32_t> readback(pl_gpu gpu, pl_tex texture)
{
    // Qualification only. The converter has no readback/CPU RGB output path.
    std::vector<uint32_t> pixels(texture->params.w * texture->params.h);
    pl_tex_transfer_params transfer{};
    transfer.tex = texture; transfer.ptr = pixels.data();
    if (!pl_tex_download(gpu, &transfer)) pixels.clear();
    return pixels;
}

int sample(const AVFrame* frame, int plane, int x, int y, int depth)
{
    const auto* row = frame->data[plane] + y * frame->linesize[plane];
    return depth == 8 ? row[x] : reinterpret_cast<const uint16_t*>(row)[x];
}
}

class SoftwareVideoTest : public QObject
{
    Q_OBJECT
    std::shared_ptr<Device> m_Device;
    pl_gpu gpu() const { return m_Device->vk->gpu; }
    pl_tex createOutput(int width, int height)
    {
        pl_tex_params params{};
        params.w = width; params.h = height;
        params.format = QtSoftwareVideoConverter::outputFormat(gpu());
        params.renderable = params.sampleable = params.host_readable = true;
        return pl_tex_create(gpu(), &params);
    }

private slots:
    void initTestCase()
    {
        m_Device = std::make_shared<Device>();
        auto params = pl_vulkan_default_params;
        params.allow_software = true; // builder-only qualification, not runtime selection
        m_Device->vk = pl_vulkan_create(nullptr, &params);
        QVERIFY2(m_Device->vk, "Requires a Vulkan test driver (lavapipe on the builder)");
        QVERIFY(PlVkFramePool::supported(gpu()));
        QVERIFY(QtSoftwareVideoConverter::outputFormat(gpu()));
    }
    void rgbCodes_data()
    {
        QTest::addColumn<int>("depth");
        QTest::newRow("rgb8") << 8;
        QTest::newRow("rgb10") << 10;
    }
    void rgbCodes()
    {
        QFETCH(int, depth);
        QtSoftwareVideoConverter converter(gpu(), m_Device);
        Context context(AV_CODEC_ID_H264); Frame frame;
        context.value->pix_fmt = depth == 8 ? AV_PIX_FMT_GBRP : AV_PIX_FMT_GBRP10LE;
        auto* f = frame.value;
        f->format = context.value->pix_fmt; f->width = 1 << depth; f->height = 8;
        f->colorspace = AVCOL_SPC_RGB; f->color_range = AVCOL_RANGE_JPEG;
        QVERIFY(converter.framePool().getBuffer(context.value, f, 0) == 0);
        Texture out{gpu(), createOutput(f->width, f->height)};
        QVERIFY(out.value);
        // Distinct RGB/gray ramps, then non-monotonic channel-independent codes.
        for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
            for (int y = 0; y < f->height; ++y) for (int x = 0; x < f->width; ++x) {
                const int max = (1 << depth) - 1;
                const int code = (x + frameIndex * 71) & max;
                const int rgb[3] = {y < 4 ? (y == 0 || y == 3 ? code : 0) : code,
                                    y < 4 ? (y == 1 || y == 3 ? code : 0) : max - code,
                                    y < 4 ? (y == 2 || y == 3 ? code : 0) : (code * 59) & max};
                for (int plane = 0; plane < 3; ++plane) {
                    const int value = rgb[(plane + 1) % 3]; // GBR plane order
                    auto* row = f->data[plane] + y * f->linesize[plane];
                    if (depth == 8) row[x] = value;
                    else reinterpret_cast<uint16_t*>(row)[x] = value;
                }
            }
            QString error;
            QVERIFY2(converter.convert(f, context.value->pix_fmt, out.value, error), qPrintable(error));
            const auto pixels = readback(gpu(), out.value);
            QCOMPARE(pixels.size(), size_t(f->width * f->height));
            for (int y = 0; y < f->height; ++y) for (int x = 0; x < f->width; ++x) {
                for (int channel = 0; channel < 3; ++channel) {
                    const int value = sample(f, (channel + 2) % 3, x, y, depth);
                    const int actual = (pixels[y * f->width + x] >> (10 * channel)) & 1023;
                    if (depth == 10) QCOMPARE(actual, value);
                    else {
                        // UNORM expansion permits either neighboring integer;
                        // the original 8-bit code must be recovered exactly.
                        QVERIFY(actual >= value * 1023 / 255 && actual <= (value * 1023 + 254) / 255);
                        QCOMPARE(qRound(actual * 255.0 / 1023.0), value);
                    }
                }
                QCOMPARE(pixels[y * f->width + x] >> 30, 3U);
            }
        }
    }
    void bt709Range_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<bool>("limited");
        QTest::newRow("422-8-full") << int(AV_PIX_FMT_YUV422P) << false;
        QTest::newRow("422-10-full") << int(AV_PIX_FMT_YUV422P10LE) << false;
        QTest::newRow("444-10-full") << int(AV_PIX_FMT_YUV444P10LE) << false;
        QTest::newRow("420-10-full") << int(AV_PIX_FMT_YUV420P10LE) << false;
        QTest::newRow("420-8-limited") << int(AV_PIX_FMT_YUV420P) << true;
        QTest::newRow("420-10-limited") << int(AV_PIX_FMT_YUV420P10LE) << true;
        QTest::newRow("444-10-limited") << int(AV_PIX_FMT_YUV444P10LE) << true;
    }
    void bt709Range()
    {
        QFETCH(int, format);
        QFETCH(bool, limited);
        QtSoftwareVideoConverter converter(gpu(), m_Device);
        Context context(AV_CODEC_ID_H264); Frame frame;
        const auto pixelFormat = static_cast<AVPixelFormat>(format);
        const auto* descriptor = av_pix_fmt_desc_get(pixelFormat);
        const int depth = descriptor->comp[0].depth;
        context.value->pix_fmt = pixelFormat;
        auto* f = frame.value;
        f->format = format; f->width = 1 << depth; f->height = 8;
        f->colorspace = AVCOL_SPC_BT709;
        f->color_range = limited ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
        QVERIFY(converter.framePool().getBuffer(context.value, f, 0) == 0);
        Texture out{gpu(), createOutput(f->width, f->height)};
        QVERIFY(out.value);
        for (int pass = 0; pass < 3; ++pass) {
            const int max = (1 << depth) - 1, center = 1 << (depth - 1);
            const int u = center + (pass == 1 ? max / 8 : 0);
            const int v = center + (pass == 2 ? max / 8 : 0);
            for (int p = 0; p < 3; ++p) {
                const int w = p ? f->width >> descriptor->log2_chroma_w : f->width;
                const int h = p ? f->height >> descriptor->log2_chroma_h : f->height;
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const int value = p == 0 ? x : p == 1 ? u : v;
                    auto* row = f->data[p] + y * f->linesize[p];
                    if (depth == 8) row[x] = value;
                    else reinterpret_cast<uint16_t*>(row)[x] = value;
                }
            }
            QString error;
            QVERIFY2(converter.convert(f, pixelFormat, out.value, error), qPrintable(error));
            const auto pixels = readback(gpu(), out.value);
            QCOMPARE(pixels.size(), size_t(f->width * f->height));
            for (int y = 0; y < f->height; ++y) for (int x = 0; x < f->width; ++x) {
                const int shift = depth - 8;
                const double yf = double(x - (limited ? 16 << shift : 0)) / (limited ? 219 << shift : max);
                const double uf = double(u - center) / (limited ? 224 << shift : max);
                const double vf = double(v - center) / (limited ? 224 << shift : max);
                const double rgb[3] = {yf + 1.5748 * vf, yf - 0.1873242729 * uf - 0.4681242729 * vf, yf + 1.8556 * uf};
                for (int channel = 0; channel < 3; ++channel) {
                    const int expected = qRound(std::clamp(rgb[channel], 0.0, 1.0) * 1023.0);
                    const int actual = (pixels[y * f->width + x] >> (10 * channel)) & 1023;
                    // Exactly represented native gray levels must not shift.
                    if (!pass && depth == 10 && !limited) QCOMPARE(actual, expected);
                    else QVERIFY2(std::abs(actual - expected) <= 1, qPrintable(QString("BT709 code %1: %2 != %3").arg(x).arg(actual).arg(expected)));
                }
            }
        }
    }
    void rejectsSubstitution()
    {
        QtSoftwareVideoConverter converter(gpu(), m_Device);
        Context context(AV_CODEC_ID_H264); Frame frame;
        auto* f = frame.value;
        context.value->pix_fmt = AV_PIX_FMT_GBRP10LE;
        f->format = context.value->pix_fmt; f->width = 64; f->height = 16;
        f->colorspace = AVCOL_SPC_RGB; f->color_range = AVCOL_RANGE_JPEG;
        QVERIFY(converter.framePool().getBuffer(context.value, f, 0) == 0);
        Texture out{gpu(), createOutput(f->width, f->height)};
        Texture resized{gpu(), createOutput(32, 16)};
        QVERIFY(out.value); QVERIFY(resized.value);
        QString error;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP, out.value, error));
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, resized.value, error));
        auto params = out.value->params;
        params.format = pl_find_named_fmt(gpu(), "rgba8");
        QVERIFY(params.format);
        Texture rgb8{gpu(), pl_tex_create(gpu(), &params)};
        QVERIFY(rgb8.value);
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, rgb8.value, error));
        f->color_range = AVCOL_RANGE_MPEG;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        f->color_range = AVCOL_RANGE_JPEG; f->colorspace = AVCOL_SPC_BT709;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        f->colorspace = AVCOL_SPC_RGB; f->color_trc = AVCOL_TRC_SMPTE2084;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        QVERIFY(!converter.convert(nullptr, AV_PIX_FMT_GBRP10LE, out.value, error));
        f->color_trc = AVCOL_TRC_BT709; f->crop_left = 1;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        f->crop_left = 0; f->flags |= AV_FRAME_FLAG_INTERLACED;
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        f->flags &= ~AV_FRAME_FLAG_INTERLACED;
        QVERIFY(av_frame_new_side_data(f, AV_FRAME_DATA_ICC_PROFILE, 128));
        QVERIFY(!converter.convert(f, AV_PIX_FMT_GBRP10LE, out.value, error));
        av_frame_remove_side_data(f, AV_FRAME_DATA_ICC_PROFILE);
        // Ordinary host memory cannot silently reintroduce a CPU upload copy.
        Frame foreign;
        foreign.value->format = f->format; foreign.value->width = 64; foreign.value->height = 16;
        foreign.value->colorspace = AVCOL_SPC_RGB; foreign.value->color_range = AVCOL_RANGE_JPEG;
        QVERIFY(av_frame_get_buffer(foreign.value, 32) == 0);
        QVERIFY(!converter.convert(foreign.value, AV_PIX_FMT_GBRP10LE, out.value, error));
        QVERIFY(error.contains("pooled"));
    }
    void chromaAndStride_data()
    {
        QTest::addColumn<int>("format"); QTest::addColumn<bool>("flipped");
        QTest::newRow("422") << int(AV_PIX_FMT_YUV422P10LE) << false;
        QTest::newRow("420") << int(AV_PIX_FMT_YUV420P10LE) << false;
        QTest::newRow("422-negative-stride") << int(AV_PIX_FMT_YUV422P10LE) << true;
        QTest::newRow("rgb-negative-stride") << int(AV_PIX_FMT_GBRP10LE) << true;
    }
    void chromaAndStride()
    {
        QFETCH(int, format); QFETCH(bool, flipped);
        QtSoftwareVideoConverter converter(gpu(), m_Device);
        Context context(AV_CODEC_ID_H264); Frame frame;
        const auto pixelFormat = static_cast<AVPixelFormat>(format);
        const auto* d = av_pix_fmt_desc_get(pixelFormat);
        const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
        auto* f = frame.value;
        context.value->pix_fmt = pixelFormat;
        f->format = format; f->width = 17; f->height = 9;
        f->colorspace = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
        f->color_range = AVCOL_RANGE_JPEG; f->chroma_location = AVCHROMA_LOC_LEFT;
        QVERIFY(converter.framePool().getBuffer(context.value, f, 0) == 0);
        for (int p = 0; p < 3; ++p) {
            const int w = AV_CEIL_RSHIFT(f->width, p ? d->log2_chroma_w : 0);
            const int h = AV_CEIL_RSHIFT(f->height, p ? d->log2_chroma_h : 0);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                reinterpret_cast<uint16_t*>(f->data[p] + y * f->linesize[p])[x] = (x * 31 + y * 71 + p * 90) & 1023;
            if (flipped) { f->data[p] += (h - 1) * f->linesize[p]; f->linesize[p] = -f->linesize[p]; }
        }
        Texture out{gpu(), createOutput(f->width, f->height)};
        QVERIFY(out.value);
        QString error;
        QVERIFY2(converter.convert(f, pixelFormat, out.value, error), qPrintable(error));
        const auto pixels = readback(gpu(), out.value);
        QCOMPARE(pixels.size(), size_t(f->width * f->height));
        for (int y = 0; y < f->height; ++y) for (int x = 0; x < f->width; ++x) {
            double values[3]{};
            for (int p = 0; p < 3; ++p) {
                const int subX = p ? 1 << d->log2_chroma_w : 1;
                const int subY = p ? 1 << d->log2_chroma_h : 1;
                const int w = (f->width + subX - 1) / subX, h = (f->height + subY - 1) / subY;
                // AVCHROMA_LOC_LEFT: horizontally co-sited; vertically centered.
                const double sx = subX > 1 ? double(x) / subX : x;
                const double sy = std::clamp((y + 0.5) / subY - 0.5, 0.0, double(h - 1));
                const int x0 = int(sx), x1 = std::min(x0 + 1, w - 1);
                const int y0 = int(sy), y1 = std::min(y0 + 1, h - 1);
                const double a = sample(f, p, x0, y0, 10) * (1 - (sx - x0)) + sample(f, p, x1, y0, 10) * (sx - x0);
                const double b = sample(f, p, x0, y1, 10) * (1 - (sx - x0)) + sample(f, p, x1, y1, 10) * (sx - x0);
                values[p] = (a * (1 - (sy - y0)) + b * (sy - y0)) / 1023.0;
            }
            const double yf = values[0], uf = values[1] - 512.0 / 1023.0, vf = values[2] - 512.0 / 1023.0;
            const double channels[] = {rgb ? values[2] : yf + 1.5748 * vf,
                                       rgb ? values[0] : yf - 0.1873242729 * uf - 0.4681242729 * vf,
                                       rgb ? values[1] : yf + 1.8556 * uf};
            for (int c = 0; c < 3; ++c) {
                const int expected = qRound(std::clamp(channels[c], 0.0, 1.0) * 1023);
                const int actual = (pixels[y * f->width + x] >> (10 * c)) & 1023;
                if (rgb) QCOMPARE(actual, expected);
                else QVERIFY2(std::abs(actual - expected) <= 1,
                              qPrintable(QString("chroma (%1,%2) channel %3: %4 != %5").arg(x).arg(y).arg(c).arg(actual).arg(expected)));
            }
        }
    }
    void decodedFixtures_data()
    {
        QTest::addColumn<int>("codec"); QTest::addColumn<QByteArray>("sample");
        using namespace PlankDecoderTestFrames;
#define SAMPLE(name, codec, data) QTest::newRow(name) << int(codec) << QByteArray(reinterpret_cast<const char*>(data), sizeof(data))
        SAMPLE("h264-8-444", AV_CODEC_ID_H264, k_h264High_444TestFrame);
        SAMPLE("h264-10-444", AV_CODEC_ID_H264, k_h264High10_444TestFrame);
        SAMPLE("h264-8-422", AV_CODEC_ID_H264, k_H264High8_422TestFrame);
        SAMPLE("h264-10-422", AV_CODEC_ID_H264, k_H264High10_422TestFrame);
        SAMPLE("hevc-8-444", AV_CODEC_ID_HEVC, k_HEVCRExt8_444IdentityGbrTestFrame);
        SAMPLE("hevc-10-444", AV_CODEC_ID_HEVC, k_HEVCRExt10_444IdentityGbrTestFrame);
#undef SAMPLE
    }
    void decodedFixtures()
    {
        QFETCH(int, codec); QFETCH(QByteArray, sample);
        QtSoftwareVideoConverter converter(gpu(), m_Device);
        Context context(static_cast<AVCodecID>(codec)); Frame frame;
        context.value->thread_count = 4;
        context.value->opaque = &converter.framePool(); context.value->get_buffer2 = getBuffer;
        QVERIFY(avcodec_open2(context.value, avcodec_find_decoder(static_cast<AVCodecID>(codec)), nullptr) == 0);
        AVPacket* packet = av_packet_alloc();
        QVERIFY(packet);
        QVERIFY(av_new_packet(packet, sample.size()) == 0);
        memcpy(packet->data, sample.data(), sample.size());
        const auto sent = avcodec_send_packet(context.value, packet);
        av_packet_free(&packet);
        QCOMPARE(sent, 0);
        QVERIFY(avcodec_send_packet(context.value, nullptr) == 0);
        QVERIFY(avcodec_receive_frame(context.value, frame.value) == 0);
        auto* f = frame.value;
        const auto format = static_cast<AVPixelFormat>(f->format);
        Texture out{gpu(), createOutput(f->width, f->height)};
        QVERIFY(out.value);
        QString error;
        QVERIFY2(converter.convert(f, format, out.value, error), qPrintable(error));
        const auto pixels = readback(gpu(), out.value);
        QCOMPARE(pixels.size(), size_t(f->width * f->height));
        const auto* descriptor = av_pix_fmt_desc_get(format);
        const int depth = descriptor->comp[0].depth;
        if (descriptor->flags & AV_PIX_FMT_FLAG_RGB) {
            for (int y = 0; y < f->height; ++y) for (int x = 0; x < f->width; ++x)
                for (int channel = 0; channel < 3; ++channel) {
                    const int input = this->sampleValue(f, (channel + 2) % 3, x, y, depth);
                    const int actual = (pixels[y * f->width + x] >> (10 * channel)) & 1023;
                    if (depth == 10) QCOMPARE(actual, input);
                    else QCOMPARE(qRound(actual * 255.0 / 1023.0), input);
                }
        }
    }
    void cleanupTestCase() { m_Device.reset(); }

private:
    // Avoid shadowing the fixture packet named `sample` above.
    static int sampleValue(const AVFrame* f, int p, int x, int y, int depth) { return sample(f, p, x, y, depth); }
};
QTEST_GUILESS_MAIN(SoftwareVideoTest)
#include "test_qtsoftwarevideo.moc"
