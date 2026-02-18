#pragma once
// Stackful coroutine (Boost.Asio spawn) based FFmpeg AVIO remux demo.
// Multiple UDP streams share one io_context thread, yielding cooperatively.

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace quink {

// Opaque context for the AVIO read callback.
// Holds a streambuf plus the socket/yield pointers needed to recv inside FFmpeg.
struct ReadContext {
    boost::asio::streambuf buf{2 * 1024 * 1024};
    boost::asio::ip::udp::socket *recv_socket = nullptr;
    boost::asio::ip::udp::endpoint *sender_ep = nullptr;
    boost::asio::yield_context *yield = nullptr;

    void bind(boost::asio::ip::udp::socket &sock,
              boost::asio::ip::udp::endpoint &ep,
              boost::asio::yield_context &y) {
        recv_socket = &sock;
        sender_ep = &ep;
        yield = &y;
    }
};

// Remux one UDP stream: recv -> demux -> remux -> send.
// Callable object designed to be passed to boost::asio::spawn().
// Kept lightweight — FFmpeg contexts are created inside Run(), not stored as members.
class RemuxCoroutine {
public:
    RemuxCoroutine(boost::asio::io_context &io_ctx, uint16_t recv_port,
                   uint16_t send_port);

    RemuxCoroutine(const RemuxCoroutine &) = delete;

    RemuxCoroutine(RemuxCoroutine &&other) noexcept : io_ctx_(other.io_ctx_),
        recv_port_(other.recv_port_),
        send_port_(other.send_port_),
        tag_(std::move(other.tag_)) {
    }

    void operator()(boost::asio::yield_context yield);

private:
    using UdpSocket = boost::asio::ip::udp::socket;
    using UdpEndpoint = boost::asio::ip::udp::endpoint;

    AVFormatContext *openInput(ReadContext &read_ctx);

    AVFormatContext *openOutput(AVFormatContext *ifmt_ctx,
                                boost::asio::streambuf &write_buf,
                                std::vector<int> &stream_map);

    void remuxLoop(AVFormatContext *ifmt_ctx, AVFormatContext *ofmt_ctx,
                   const std::vector<int> &stream_map,
                   boost::asio::streambuf &write_buf, UdpSocket &send_socket,
                   const UdpEndpoint &send_dest,
                   boost::asio::yield_context &yield);

    void flushWriteBuffer(boost::asio::streambuf &wb, UdpSocket &socket,
                          const UdpEndpoint &dest,
                          boost::asio::yield_context &yield);

    boost::asio::io_context &io_ctx_;
    uint16_t recv_port_;
    uint16_t send_port_;
    std::string tag_;
};
} // namespace quink
