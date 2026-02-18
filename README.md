# FFmpeg avio with Boost.Asio Stackful Coroutine Remux Demo

Demonstrates **true single-threaded concurrent processing** of multiple video
stream remuxing pipelines using **Boost.Asio stackful coroutines
(`boost::asio::spawn` + `yield_context`) + FFmpeg custom AVIO callbacks**.

## Architecture

```
UDP recv (even port, e.g. 9000)
    │  yield_context suspends/resumes
    ▼
ReadContext (streambuf) → AVIO read_packet (sync callback)
                            │  if buffer empty: YIELD entire FFmpeg call stack
                            │  → io_context runs other coroutines
                            │  → UDP data arrives → resume exactly here
                            ▼
                        avformat demuxer (e.g. MPEG-TS → H.264 packets)
                            │
                            ▼
                        avformat muxer  (H.264 packets → MPEG-TS)
                            │
                            ▼
                        AVIO write_packet (sync callback) → streambuf
                                                               │
                                                               ▼
                                               yield_context async_send_to
                                               UDP send (odd port, e.g. 9001)
```

**Key advantage over C++20 stackless coroutines**: Boost.Asio stackful
coroutines can suspend at ANY call depth — including inside FFmpeg's
`read_packet` callback. This means:

- **No separate demux thread** — everything runs on one OS thread
- **No mutex / condition_variable** — pure cooperative scheduling
- **N streams share ONE thread** via the `io_context` event loop

### Why Not C++20 Stackless Coroutines?

C++20 stackless coroutines (`co_await`) cannot suspend inside FFmpeg's C
callback functions — they can only suspend at explicit `co_await` points.
This forces a two-thread architecture (demux thread + I/O coroutine) with
mutex/condvar synchronization, defeating the single-threaded goal.

Stackful coroutines (`boost::asio::spawn`) give each coroutine its own
stack, so the yield can happen deep inside `read_packet → fill_buffer →
av_read_frame`, preserving the entire FFmpeg call stack across suspensions.

## Dependencies

- C++17 compiler
- Boost (context + coroutine components)
- FFmpeg libraries: libavformat, libavcodec, libavutil (via pkg-config)

## Build

```bash
cmake -B build && cmake --build build
```

## Usage

```bash
# Start the remuxer (even port receives, odd port sends)
./avio_coroutine 9000

# Multiple pipelines (all on one thread!):
./avio_coroutine 9000 9002 9004
# Creates: 9000→9001, 9002→9003, 9004→9005
```

### Verification with ffmpeg CLI

```bash
# Terminal 1: start the remuxer
./avio_coroutine 9000

# Terminal 2: push video in MPEG-TS via UDP
ffmpeg -re -i input.mp4 -c copy -f mpegts -pkt_size 1316 udp://127.0.0.1:9000

# Terminal 3: play the remuxed output
ffplay udp://127.0.0.1:9001
```

## Technical Details

### AVIO Callback — Yielding Inside FFmpeg

The AVIO `read_packet` callback uses a `ReadContext` that wraps a
`boost::asio::streambuf` together with a UDP socket and `yield_context`.
When the buffer is empty, the callback yields the coroutine to receive
UDP data — suspending the entire FFmpeg call stack.
