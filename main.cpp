/* FFmpeg AVIO + Boost.Asio Stackful Coroutine (spawn) Demo
 *
 * Single thread concurrent processing of multiple video stream.
 *
 * Remux pipelines using Boost.Asio stackful coroutines (spawn/yield_context)
 * + FFmpeg AVIO callbacks.
 *
 * Key advantage over C++20 stackless coroutines:
 *   Stackful coroutines can yield inside FFmpeg's AVIO callback, because each
 *   coroutine has its own stack.
 *
 * Usage:
 *   ./avio_coroutine 9000 9002 9004
 *
 * Verification:
 *   # start the remuxer
 *   ./avio_coroutine 9000 9002
 *
 *   # start two processes and ingest to port 9000 and 9002
 *   ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
 *          -c:v libx264 -preset superfast -tune zerolatency -g 30 \
 *          -f mpegts -pkt_size 1316 udp://127.0.0.1:9000
 *   ffmpeg -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
 *          -c:v libx264 -preset superfast -tune zerolatency -g 30 \
 *          -f mpegts -pkt_size 1316 udp://127.0.0.1:9002
 *
 *   # playback
 *   ffplay udp://127.0.0.1:9001
 *   ffplay udp://127.0.0.1:9003
 */

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "remux.h"

static void printUsage(const char *prog) {
    std::cout
            << "Usage: " << prog << " <even_port1> [even_port2] ...\n"
            << "\n"
            << "  Each even port creates a remux pipeline:\n"
            << "    even_port (recv) -> demux -> remux -> even_port+1 (send)\n"
            << "\n"
            << "  TRUE SINGLE-THREADED: All streams share one thread via\n"
            << "  Boost.Asio stackful coroutines (spawn/yield_context).\n"
            << "\n"
            << "Examples:\n"
            << "  " << prog << " 9000           # pipeline: 9000 -> 9001\n"
            << "  " << prog <<
            " 9000 9002      # two pipelines: 9000->9001, 9002->9003\n"
            << std::endl;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    std::vector<uint16_t> recv_ports;
    for (int i = 1; i < argc; i++) {
        try {
            int p = std::stoi(argv[i]);
            if (p < 1 || p > 65534) {
                std::cerr << "Invalid port: " << argv[i] << std::endl;
                return EXIT_FAILURE;
            }
            if (p % 2 != 0) {
                std::cerr << "Port must be even: " << argv[i] << std::endl;
                return EXIT_FAILURE;
            }
            recv_ports.push_back(static_cast<uint16_t>(p));
        } catch (...) {
            std::cerr << "Invalid port: " << argv[i] << std::endl;
            return EXIT_FAILURE;
        }
    }

    boost::asio::io_context io_ctx;
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int sig) {
        std::cout << "\nReceived signal " << sig << ", shutting down..." <<
                std::endl;
        io_ctx.stop();
    });

    for (uint16_t recv_port : recv_ports) {
        uint16_t send_port = recv_port + 1;
        boost::asio::spawn(io_ctx, quink::RemuxCoroutine{io_ctx, recv_port, send_port},
                           boost::asio::detached);
    }

    std::cout << "========================================" << std::endl;
    std::cout << " AVIO Boost.Asio Stackful Coroutine Demo" << std::endl;
    std::cout << " Single-threaded, " << recv_ports.size() <<
            " remux pipeline(s)" << std::endl;
    for (uint16_t rp : recv_ports) {
        std::cout << "   UDP :" << rp << " (recv) -> :" << rp + 1 << " (send)"
                << std::endl;
    }
    std::cout << " Press Ctrl+C to stop" << std::endl;
    std::cout << "========================================" << std::endl;

    // Single-threaded event loop — all coroutines yield cooperatively.
    io_ctx.run();

    std::cout << "Done." << std::endl;

    return EXIT_SUCCESS;
}
