#include "remux.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace quink {
// ---------------------------------------------------------------------------
// RAII deleters for FFmpeg types (via std::unique_ptr)
// ---------------------------------------------------------------------------

struct AvioContextDeleter {
    void operator()(AVIOContext *ctx) const {
        if (ctx) {
            av_freep(&ctx->buffer);
            avio_context_free(&ctx);
        }
    }
};

struct InputFormatDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx) avformat_close_input(&ctx);
    }
};

struct OutputFormatDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx) avformat_free_context(ctx);
    }
};

struct PacketDeleter {
    void operator()(AVPacket *pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

using AvioContextPtr = std::unique_ptr<AVIOContext, AvioContextDeleter>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// AVIO callbacks
// Read callback: pull data from ReadContext (streambuf + async UDP recv).
// When the buffer is empty, yields the coroutine to wait for UDP data.
static int readPacket(void *opaque, uint8_t *buf, int buf_size) {
    auto *rc = static_cast<ReadContext *>(opaque);

    // If buffer is empty, yield to receive UDP data
    while (rc->buf.size() == 0) {
        if (!rc->recv_socket || !rc->yield) return AVERROR_EOF;

        boost::system::error_code ec;
        size_t n = rc->recv_socket->async_receive_from(
            rc->buf.prepare(65536), *rc->sender_ep, (*rc->yield)[ec]);
        if (ec || n == 0) return AVERROR_EOF;
        rc->buf.commit(n);
    }

    size_t avail = rc->buf.size();
    size_t to_copy = std::min(avail, static_cast<size_t>(buf_size));
    auto readable = rc->buf.data();
    std::memcpy(buf, readable.data(), to_copy);
    rc->buf.consume(to_copy);
    return static_cast<int>(to_copy);
}

// Write callback: push muxer output into a streambuf.
static int writePacket(void *opaque, const uint8_t *buf, int buf_size) {
    auto *wb = static_cast<boost::asio::streambuf *>(opaque);
    auto dest = wb->prepare(static_cast<size_t>(buf_size));
    std::memcpy(dest.data(), buf, static_cast<size_t>(buf_size));
    wb->commit(static_cast<size_t>(buf_size));
    return buf_size;
}

static AvioContextPtr makeReadAvio(ReadContext *rc,
                                   size_t io_buf_size = 32768) {
    auto *io_buf = static_cast<unsigned char *>(av_malloc(io_buf_size));
    if (!io_buf) return nullptr;
    AVIOContext *ctx = avio_alloc_context(
        io_buf, static_cast<int>(io_buf_size),
        0, rc, readPacket, nullptr, nullptr);
    if (!ctx) {
        av_free(io_buf);
        return nullptr;
    }
    return AvioContextPtr(ctx);
}

static AvioContextPtr makeWriteAvio(boost::asio::streambuf *wb,
                                    size_t io_buf_size = 188 * 24) {
    auto *io_buf = static_cast<unsigned char *>(av_malloc(io_buf_size));
    if (!io_buf) return nullptr;
    AVIOContext *ctx = avio_alloc_context(
        io_buf, static_cast<int>(io_buf_size),
        1, wb, nullptr, writePacket, nullptr);
    if (!ctx) {
        av_free(io_buf);
        return nullptr;
    }
    return AvioContextPtr(ctx);
}

static std::string avErr(int err) {
    char msg[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, msg, sizeof(msg));
    return msg;
}

RemuxCoroutine::RemuxCoroutine(boost::asio::io_context &io_ctx,
                               uint16_t recv_port, uint16_t send_port)
    : io_ctx_(io_ctx), recv_port_(recv_port), send_port_(send_port),
      tag_("[Remux:" + std::to_string(recv_port) + "->" +
           std::to_string(send_port) + "]") {
}

void RemuxCoroutine::operator()(boost::asio::yield_context yield) {
    using udp = boost::asio::ip::udp;

    // UDP sockets
    udp::socket recv_socket(io_ctx_, udp::v4());
    recv_socket.set_option(boost::asio::socket_base::reuse_address(true));
    recv_socket.bind(udp::endpoint(udp::v4(), recv_port_));

    udp::socket send_socket(io_ctx_, udp::v4());
    udp::endpoint send_dest(boost::asio::ip::make_address("127.0.0.1"),
                            send_port_);
    udp::endpoint sender_ep;

    std::cout << tag_ << " Listening on :" << recv_port_
            << ", forwarding to :" << send_port_ << std::endl;

    // Read side: streambuf-based context for AVIO callback
    ReadContext read_ctx;
    read_ctx.bind(recv_socket, sender_ep, yield);

    // Open input (probing yields for UDP data automatically)
    auto *ifmt_ctx = openInput(read_ctx);
    if (!ifmt_ctx) return;
    InputFormatPtr ifmt_guard(ifmt_ctx);

    // Open output
    boost::asio::streambuf write_buf;
    std::vector<int> stream_map;
    auto *ofmt_ctx = openOutput(ifmt_ctx, write_buf, stream_map);
    if (!ofmt_ctx) return;
    OutputFormatPtr ofmt_guard(ofmt_ctx);

    // Flush muxer header to UDP
    flushWriteBuffer(write_buf, send_socket, send_dest, yield);

    std::cout << tag_ << " Remuxing..." << std::endl;

    // Main loop
    remuxLoop(ifmt_ctx, ofmt_ctx, stream_map, write_buf,
              send_socket, send_dest, yield);

    // Trailer + final flush
    av_write_trailer(ofmt_ctx);
    flushWriteBuffer(write_buf, send_socket, send_dest, yield);
}

AVFormatContext *RemuxCoroutine::openInput(ReadContext &read_ctx) {
    auto in_avio = makeReadAvio(&read_ctx);
    if (!in_avio) {
        std::cerr << tag_ << " Failed to create input AVIO" << std::endl;
        return nullptr;
    }

    AVFormatContext *ifmt_ctx = avformat_alloc_context();
    ifmt_ctx->pb = in_avio.get();
    ifmt_ctx->probesize = 512 * 1024;
    ifmt_ctx->max_analyze_duration = 1000000;

    std::cout << tag_ << " Waiting for input stream..." << std::endl;

    int ret = avformat_open_input(&ifmt_ctx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        std::cerr << tag_ << " avformat_open_input: " << avErr(ret) <<
                std::endl;
        return nullptr;
    }
    // AVIO ownership transferred to ifmt_ctx
    in_avio.release();

    ret = avformat_find_stream_info(ifmt_ctx, nullptr);
    if (ret < 0) {
        std::cerr << tag_ << " avformat_find_stream_info: " << avErr(ret) <<
                std::endl;
        avformat_close_input(&ifmt_ctx);
        return nullptr;
    }

    if (ifmt_ctx->nb_streams == 0) {
        std::cerr << tag_ << " No streams found" << std::endl;
        avformat_close_input(&ifmt_ctx);
        return nullptr;
    }

    return ifmt_ctx;
}

AVFormatContext *RemuxCoroutine::openOutput(AVFormatContext *ifmt_ctx,
                                            boost::asio::streambuf &write_buf,
                                            std::vector<int> &stream_map) {
    auto out_avio = makeWriteAvio(&write_buf);
    if (!out_avio) {
        std::cerr << tag_ << " Failed to create output AVIO" << std::endl;
        return nullptr;
    }

    AVFormatContext *ofmt_ctx = nullptr;
    int ret = avformat_alloc_output_context2(&ofmt_ctx, nullptr, "mpegts",
                                             nullptr);
    if (ret < 0 || !ofmt_ctx) {
        std::cerr << tag_ << " Failed to alloc output context" << std::endl;
        return nullptr;
    }
    ofmt_ctx->pb = out_avio.get();
    out_avio.release(); // ownership to ofmt_ctx

    stream_map.assign(ifmt_ctx->nb_streams, -1);
    int out_idx = 0;
    for (unsigned i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (!out_stream) {
            avformat_free_context(ofmt_ctx);
            return nullptr;
        }
        avcodec_parameters_copy(out_stream->codecpar,
                                ifmt_ctx->streams[i]->codecpar);
        stream_map[i] = out_idx++;
    }

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        std::cerr << tag_ << " avformat_write_header: " << avErr(ret) <<
                std::endl;
        avformat_free_context(ofmt_ctx);
        return nullptr;
    }

    return ofmt_ctx;
}

void RemuxCoroutine::remuxLoop(AVFormatContext *ifmt_ctx,
                               AVFormatContext *ofmt_ctx,
                               const std::vector<int> &stream_map,
                               boost::asio::streambuf &write_buf,
                               UdpSocket &send_socket,
                               const UdpEndpoint &send_dest,
                               boost::asio::yield_context &yield) {
    PacketPtr pkt(av_packet_alloc());
    uint64_t total_pkts = 0, total_bytes_sent = 0;
    while (true) {
        int ret = av_read_frame(ifmt_ctx, pkt.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                std::cout << tag_ << " EOF" << std::endl;
            else
                std::cerr << tag_ << ' ' << avErr(ret) << std::endl;
            break;
        }

        int in_idx = pkt->stream_index;
        if (in_idx < 0 || in_idx >= static_cast<int>(stream_map.size())
            || stream_map[in_idx] < 0) {
            av_packet_unref(pkt.get());
            continue;
        }

        AVStream *in_stream = ifmt_ctx->streams[in_idx];
        int out_idx = stream_map[in_idx];
        AVStream *out_stream = ofmt_ctx->streams[out_idx];

        pkt->stream_index = out_idx;
        av_packet_rescale_ts(pkt.get(), in_stream->time_base,
                             out_stream->time_base);
        total_pkts++;
        bool is_video = (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        if (is_video && (pkt->flags & AV_PKT_FLAG_KEY)) {
            std::cout << tag_ << " Keyframe pts=" << pkt->pts
                    << " size=" << pkt->size
                    << " total=" << total_pkts
                    << " sent=" << total_bytes_sent << "B" << std::endl;
        } else if (total_pkts % 500 == 1) {
            const char *type = av_get_media_type_string(
                in_stream->codecpar->codec_type);
            std::cout << tag_ << " #" << total_pkts
                    << " " << (type ? type : "?")
                    << " pts=" << pkt->pts << std::endl;
        }

        av_interleaved_write_frame(ofmt_ctx, pkt.get());
        if (write_buf.size() > 0) {
            size_t pending = write_buf.size();
            flushWriteBuffer(write_buf, send_socket, send_dest, yield);
            total_bytes_sent += pending;
        }
    }

    std::cout << tag_ << " Done. Packets: " << total_pkts << std::endl;
}

// send TS-aligned chunks over UDP
void RemuxCoroutine::flushWriteBuffer(boost::asio::streambuf &wb,
                                      UdpSocket &socket,
                                      const UdpEndpoint &dest,
                                      boost::asio::yield_context &yield) {
    constexpr size_t ts_packet_size = 188;
    constexpr size_t ts_udp_chunk = ts_packet_size * 7; // 1316 bytes

    size_t aligned = (wb.size() / ts_packet_size) * ts_packet_size;
    while (aligned > 0) {
        size_t to_send = std::min(aligned, ts_udp_chunk);
        boost::system::error_code ec;
        socket.async_send_to(
            boost::asio::buffer(wb.data().data(), to_send),
            dest, yield[ec]);
        if (ec) {
            std::cerr << tag_ << " UDP send error: " << ec.message() <<
                    std::endl;
            return;
        }
        wb.consume(to_send);
        aligned -= to_send;
    }
}

} // namespace quink
