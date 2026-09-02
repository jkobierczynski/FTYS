// Smoke tests for the io library: writes tiny synthetic SER, FITS, and AVI
// files with a bright disk that shifts a few pixels per frame (representative
// of a wobbling planetary capture), then reads them back through
// openFrameSource() and checks basic invariants. Not a substitute for testing
// against real capture files, but catches header/parsing regressions cheaply.

#include "core/FrameSource.h"
#include "core/ImageBuffer.h"
#include "TestTempDir.h"

#include <fitsio.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        g_failures++;
    } else {
        std::cerr << "ok:   " << msg << "\n";
    }
}

constexpr int W = 32, H = 24, NFRAMES = 5;

// Fills an 8-bit mono buffer with a bright disk centered at (cx, cy).
void renderDisk(std::vector<uint8_t>& buf, int w, int h, double cx, double cy, double radius) {
    buf.assign(static_cast<size_t>(w) * h, 10); // dim background
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) buf[static_cast<size_t>(y) * w + x] = 220;
        }
    }
}

void writeI32LE(std::ofstream& f, int32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void writeI64LE(std::ofstream& f, int64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); }

std::string writeSyntheticSer(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    char fileId[14] = {'L', 'U', 'C', 'A', 'M', '-', 'R', 'E', 'C', 'O', 'R', 'D', 'E', 'R'};
    f.write(fileId, 14);
    writeI32LE(f, 0);           // LuID
    writeI32LE(f, 0);           // ColorID = MONO
    writeI32LE(f, 1);           // LittleEndian
    writeI32LE(f, W);
    writeI32LE(f, H);
    writeI32LE(f, 8);           // PixelDepth
    writeI32LE(f, NFRAMES);
    char pad40[40] = {0};
    f.write(pad40, 40); f.write(pad40, 40); f.write(pad40, 40); // Observer/Instrument/Telescope
    writeI64LE(f, 0); writeI64LE(f, 0); // DateTime, DateTime_UTC

    for (int i = 0; i < NFRAMES; ++i) {
        std::vector<uint8_t> buf;
        renderDisk(buf, W, H, W / 2.0 + i * 0.7, H / 2.0 - i * 0.4, 6.0);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    return path;
}

std::string writeSyntheticFits(const std::string& path) {
    std::remove(path.c_str());
    fitsfile* fptr = nullptr;
    int status = 0;
    fits_create_file(&fptr, path.c_str(), &status);
    long naxes[3] = {W, H, NFRAMES};
    fits_create_img(fptr, USHORT_IMG, 3, naxes, &status);

    std::vector<uint16_t> cube(static_cast<size_t>(W) * H * NFRAMES);
    for (int i = 0; i < NFRAMES; ++i) {
        std::vector<uint8_t> buf;
        renderDisk(buf, W, H, W / 2.0 + i * 0.7, H / 2.0 - i * 0.4, 6.0);
        for (int p = 0; p < W * H; ++p)
            cube[static_cast<size_t>(i) * W * H + p] = static_cast<uint16_t>(buf[p]) * 200; // scale into 16-bit range
    }
    long fpixel[3] = {1, 1, 1};
    fits_write_pix(fptr, TUSHORT, fpixel, cube.size(), cube.data(), &status);
    fits_close_file(fptr, &status);
    if (status) {
        char errText[FLEN_ERRMSG];
        fits_get_errstatus(status, errText);
        std::cerr << "FITS write error: " << errText << "\n";
    }
    return path;
}

std::string writeSyntheticAvi(const std::string& path) {
    AVFormatContext* fmtCtx = nullptr;
    avformat_alloc_output_context2(&fmtCtx, nullptr, "avi", path.c_str());
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO);
    AVStream* stream = avformat_new_stream(fmtCtx, nullptr);
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->width = W;
    codecCtx->height = H;
    codecCtx->pix_fmt = AV_PIX_FMT_GRAY8;
    codecCtx->time_base = {1, 25};
    stream->time_base = codecCtx->time_base;
    avcodec_open2(codecCtx, codec, nullptr);
    avcodec_parameters_from_context(stream->codecpar, codecCtx);

    // Both of these are declared warn_unused_result in current FFmpeg
    // headers -- this is test-fixture-writing code, not a place that needs
    // elaborate recovery, but an unchecked failure here would otherwise
    // silently produce a corrupt/empty AVI and fail confusingly deep inside
    // the actual test that reads it back, so just throw immediately.
    if (avio_open(&fmtCtx->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
        throw std::runtime_error("writeSyntheticAvi: avio_open failed for '" + path + "'");
    if (avformat_write_header(fmtCtx, nullptr) < 0)
        throw std::runtime_error("writeSyntheticAvi: avformat_write_header failed for '" + path + "'");

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_GRAY8;
    frame->width = W;
    frame->height = H;
    av_frame_get_buffer(frame, 0);

    for (int i = 0; i < NFRAMES; ++i) {
        std::vector<uint8_t> buf;
        renderDisk(buf, W, H, W / 2.0 + i * 0.7, H / 2.0 - i * 0.4, 6.0);
        av_frame_make_writable(frame);
        std::memcpy(frame->data[0], buf.data(), buf.size());
        frame->pts = i;

        AVPacket* pkt = av_packet_alloc();
        if (avcodec_send_frame(codecCtx, frame) == 0) {
            while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                pkt->stream_index = stream->index;
                av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
                av_interleaved_write_frame(fmtCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
    }
    // flush
    avcodec_send_frame(codecCtx, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx, pkt) == 0) {
        pkt->stream_index = stream->index;
        av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
        av_interleaved_write_frame(fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(fmtCtx);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    avio_closep(&fmtCtx->pb);
    avformat_free_context(fmtCtx);
    return path;
}

void testSer() {
    std::string path = writeSyntheticSer(ls::test::tempPath("ls_test.ser"));
    auto src = ls::openFrameSource(path);
    expect(src->width() == W, "SER width");
    expect(src->height() == H, "SER height");
    expect(src->frameCount() == NFRAMES, "SER frame count");
    expect(src->format() == ls::PixelFormat::Mono8, "SER format is Mono8");

    ls::RawFrame f0 = src->readFrame(0);
    ls::ImageBuffer buf = ls::imageBufferFromRaw(f0);
    float center = buf.at(W / 2, H / 2, 0);
    expect(center > 0.5f, "SER frame 0 center pixel is bright");

    ls::RawFrame f4 = src->readFrame(4);
    ls::ImageBuffer buf4 = ls::imageBufferFromRaw(f4);
    expect(std::fabs(buf4.at(W / 2, H / 2, 0) - buf.at(W / 2, H / 2, 0)) < 2.0f, "SER frame 4 reads without throwing / plausible values");
}

void testFits() {
    std::string path = writeSyntheticFits(ls::test::tempPath("ls_test.fits"));
    auto src = ls::openFrameSource(path);
    expect(src->width() == W, "FITS width");
    expect(src->height() == H, "FITS height");
    expect(src->frameCount() == NFRAMES, "FITS frame count (cube depth)");
    expect(src->format() == ls::PixelFormat::Mono16, "FITS format is Mono16");

    ls::RawFrame f0 = src->readFrame(0);
    ls::ImageBuffer buf = ls::imageBufferFromRaw(f0);
    expect(buf.at(W / 2, H / 2, 0) > 0.5f, "FITS frame 0 center pixel is bright");
}

void testAvi() {
    std::string path = writeSyntheticAvi(ls::test::tempPath("ls_test.avi"));
    auto src = ls::openFrameSource(path);
    expect(src->width() == W, "AVI width");
    expect(src->height() == H, "AVI height");
    expect(src->frameCount() == NFRAMES, "AVI frame count == " + std::to_string(NFRAMES) + " (got " + std::to_string(src->frameCount()) + ")");
    expect(src->format() == ls::PixelFormat::Mono8, "AVI format is Mono8");

    ls::RawFrame f0 = src->readFrame(0);
    ls::ImageBuffer buf = ls::imageBufferFromRaw(f0);
    expect(buf.at(W / 2, H / 2, 0) > 0.5f, "AVI frame 0 center pixel is bright");

    ls::RawFrame f2 = src->readFrame(2);
    ls::ImageBuffer buf2 = ls::imageBufferFromRaw(f2);
    expect(buf2.width() == W && buf2.height() == H, "AVI frame 2 decodes to correct size");
}

} // namespace

int main() {
    testSer();
    testFits();
    testAvi();
    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "all io tests passed\n";
    return 0;
}
