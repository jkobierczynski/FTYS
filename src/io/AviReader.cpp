#include "io/AviReader.h"

#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
}

namespace ls {

AviReader::AviReader(const std::string& path) : FrameSourceBase(path) {
    if (avformat_open_input(&fmtCtx_, path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("AviReader: cannot open '" + path + "'");

    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0) {
        avformat_close_input(&fmtCtx_);
        throw std::runtime_error("AviReader: cannot read stream info in '" + path + "'");
    }

    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        if (fmtCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex_ = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex_ < 0) {
        avformat_close_input(&fmtCtx_);
        throw std::runtime_error("AviReader: no video stream in '" + path + "'");
    }

    AVCodecParameters* params = fmtCtx_->streams[videoStreamIndex_]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) {
        avformat_close_input(&fmtCtx_);
        throw std::runtime_error("AviReader: no decoder for codec in '" + path + "'");
    }

    codecCtx_ = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codecCtx_, params);
    if (avcodec_open2(codecCtx_, decoder, nullptr) < 0) {
        throw std::runtime_error("AviReader: failed to open decoder for '" + path + "'");
    }

    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    if (width_ <= 0 || height_ <= 0)
        throw std::runtime_error("AviReader: invalid dimensions in '" + path + "'");

    // Grayscale source codecs (Y800/Y8/GRAY8 style) decode straight to a
    // single-component pixel format; anything else we normalize to RGB24.
    // PAL8's single component is a palette INDEX, not a sample value, but
    // it still reports nb_components == 1 -- treating it as mono_ here is
    // fine and intentional, since the actual color is resolved through
    // palette_ below before sws_scale ever sees the index bytes.
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codecCtx_->pix_fmt);
    mono_ = desc && desc->nb_components == 1;
    format_ = mono_ ? PixelFormat::Mono8 : PixelFormat::RGB24;

    // Copy out the AVI's global color table now, from position-independent
    // stream metadata, rather than relying on the packet-level side data /
    // decoder-cached palette that readFrame()'s seek+flush destroys -- see
    // the class comment in AviReader.h for why.
    isPal8_ = (codecCtx_->pix_fmt == AV_PIX_FMT_PAL8);
    if (isPal8_ && codecCtx_->extradata && codecCtx_->extradata_size >= static_cast<int>(256 * sizeof(uint32_t))) {
        palette_.resize(256);
        std::memcpy(palette_.data(), codecCtx_->extradata, 256 * sizeof(uint32_t));
    } else if (isPal8_) {
        throw std::runtime_error("AviReader: PAL8 stream in '" + path + "' has no palette in stream metadata");
    }

    scanFrames();
}

AviReader::~AviReader() {
    if (swsCtx_) sws_freeContext(swsCtx_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
}

void AviReader::scanFrames() {
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmtCtx_, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex_) {
            int64_t pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            ptsList_.push_back(pts);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    frameCount_ = ptsList_.size();
    if (frameCount_ == 0) throw std::runtime_error("AviReader: no decodable video frames in '" + path_ + "'");
}

RawFrame AviReader::readFrame(size_t index) {
    if (index >= ptsList_.size()) throw std::runtime_error("AviReader: frame index out of range");
    int64_t targetPts = ptsList_[index];

    // The pipeline reads frames in ascending index order during both the
    // quality pass (every frame, strictly +1) and alignment/stacking (the
    // selected subset, ascending but with gaps), so seeking before every
    // single frame -- what the naive version of this function did --
    // meant paying a seek-and-flush cost per frame even when the packets
    // we wanted were already coming up next in the stream. That measured
    // at ~50ms/frame on a real 846-frame capture. The read loop below
    // already discards any decoded frame whose pts is short of the
    // target, so as long as we're moving forward we can skip the seek
    // entirely and let it walk over the gap -- only a backward or
    // first-ever access needs an actual seek.
    bool movingForward = lastDecodedIndex_ >= 0 && index > static_cast<size_t>(lastDecodedIndex_);
    if (!movingForward) {
        if (av_seek_frame(fmtCtx_, videoStreamIndex_, targetPts, AVSEEK_FLAG_BACKWARD) < 0)
            throw std::runtime_error("AviReader: seek failed for frame " + std::to_string(index));
        avcodec_flush_buffers(codecCtx_);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    RawFrame out;

    // All-intra assumption: after a backward seek we should hit the target
    // (or something before it) within a handful of packets; guard with a
    // generous bound so a malformed file errors instead of looping forever.
    int guard = 0;
    while (!got && guard++ < 4096) {
        int readRet = av_read_frame(fmtCtx_, pkt);
        if (readRet < 0) break;
        if (pkt->stream_index != videoStreamIndex_) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            int64_t fpts = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp
                                                                             : frame->pts;
            if (fpts >= targetPts) {
                // Force our own known-good palette onto this frame rather
                // than trust whatever (possibly empty, post-seek) palette
                // the decoder itself attached -- see the class comment in
                // AviReader.h. data[1]/linesize[1] is exactly where
                // sws_scale expects to find a PAL8 frame's color table.
                if (isPal8_) {
                    frame->data[1] = reinterpret_cast<uint8_t*>(palette_.data());
                    frame->linesize[1] = 0;
                }
                AVPixelFormat dstFmt = mono_ ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_RGB24;
                if (!swsCtx_) {
                    swsCtx_ = sws_getContext(width_, height_, static_cast<AVPixelFormat>(frame->format),
                                              width_, height_, dstFmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!swsCtx_) throw std::runtime_error("AviReader: failed to create scaler");
                }
                out.width = width_;
                out.height = height_;
                out.format = format_;
                int channels = mono_ ? 1 : 3;
                out.bytes.resize(static_cast<size_t>(width_) * height_ * channels);
                uint8_t* dstSlices[4] = {out.bytes.data(), nullptr, nullptr, nullptr};
                int dstStrides[4] = {width_ * channels, 0, 0, 0};
                sws_scale(swsCtx_, frame->data, frame->linesize, 0, height_, dstSlices, dstStrides);
                got = true;
                break;
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (!got) {
        // Leave the reader's notion of "where we are" unambiguous: force a
        // real seek on the next call rather than risk assuming sequential
        // continuation from a decoder state we're not sure about.
        lastDecodedIndex_ = -1;
        throw std::runtime_error("AviReader: failed to decode frame " + std::to_string(index));
    }
    lastDecodedIndex_ = static_cast<int64_t>(index);
    return out;
}

} // namespace ls
