# 用有栈协程突破 FFmpeg AVIO 回调的多路并发瓶颈

## 引言

FFmpeg 是音视频领域事实上的标准库。当我们需要在应用中自定义 I/O 数据源时，FFmpeg 提供了 AVIO 自定义回调机制。然而这个机制是同步阻塞式的 C 函数设计，这给单线程并发处理多路流带来了挑战。

这篇文章介绍一种思路——利用**有栈协程**（stackful coroutine）的特性，在 AVIO 回调内部透明地挂起和恢复执行，从而在**单线程**内实现多路流的并发处理。文章以 UDP 传输 MPEG-TS 流的 remux 为例进行说明，这是最简单的演示场景；同样的原理适用于任何需要自定义 I/O 数据源的 FFmpeg 应用。

## FFmpeg 的两种 I/O 方式

FFmpeg 的 `libavformat` 库负责容器格式的解封装（demux）和封装（mux）。它需要从某个"地方"读取数据、把数据写到某个"地方"。为此 FFmpeg 提供了两种 I/O 途径：

### 内置协议 I/O

最常见的用法是把 URL 直接交给 FFmpeg：

```c
avformat_open_input(&fmt_ctx, "udp://127.0.0.1:9000", NULL, NULL);
```

FFmpeg 内部维护了一套协议层（`libavformat/protocols`），支持 file、http、udp、tcp、rtmp 等。这种方式下，I/O 完全由 FFmpeg 控制——`av_read_frame()` 内部最终调到操作系统的 `recv()`、`read()` 等系统调用。

这种方式简单直接，但调用者对 I/O 过程没有控制力。

### AVIO 自定义回调

FFmpeg 提供了 `AVIOContext`，允许调用者**自己实现**数据的读写逻辑：

```c
AVIOContext* avio = avio_alloc_context(
    buffer,         // I/O 缓冲区
    buffer_size,    // 缓冲区大小
    0,              // 0=读，1=写
    opaque,         // 传给回调的用户数据指针
    read_packet,    // 读回调函数
    write_packet,   // 写回调函数
    seek            // seek 回调函数
);
fmt_ctx->pb = avio;
```

当 `pb` 被手动赋值后，FFmpeg 会自动设置 `AVFMT_FLAG_CUSTOM_IO` 标志，无需手动指定。之后调用 `avformat_open_input()` / `av_read_frame()` 时，FFmpeg 不再使用内置协议层，而是通过 `read_packet` 回调从调用者提供的数据源中获取数据。同理 mux 阶段通过 `write_packet` 回调把封装好的数据交给调用者。

AVIO 回调让调用者可以自由决定数据来源，例如从内存缓冲区、自定义网络协议、或者加密管道中读取数据。

## AVIO 回调的本质：同步阻塞式 C 函数

理解 AVIO 回调的设计是后续讨论的基础。

`read_packet` 的签名是：

```c
int read_packet(void *opaque, uint8_t *buf, int buf_size);
```

这是一个**同步阻塞式的 C 函数调用**。当 FFmpeg 内部需要数据时（例如 `av_read_frame` → `fill_buffer` → `read_packet`），它会**直接调用**这个函数指针，并**等待它返回**。整个调用链是：

```
av_read_frame()
  └─ avformat 内部解析逻辑（如 mpegts_read_packet）
       └─ fill_buffer() / avio_read()
            └─ read_packet()  ← 你的回调，FFmpeg 等它返回
```

关键约束在于：**`read_packet` 回调不能返回 `AVERROR(EAGAIN)`**。

一方面，AVIOContext 缓冲层（`aviobuf.c` 的 `fill_buffer`）不区分 EAGAIN 和其他错误——任何负返回值都会导致 `eof_reached = 1`，使 AVIOContext 进入无法自动恢复的错误状态。

但更根本的问题是：**即使 FFmpeg 正确处理了 EAGAIN，它也只是在内部立即重试 `read_packet` 调用**——事实上 FFmpeg 内置协议层对底层 I/O 的 EAGAIN 就是这样处理的：busy-retry 直到拿到数据。这意味着控制权始终在 FFmpeg 的调用栈内部循环，**永远不会返回到我们的事件循环**，也就没有机会去调度其他协程、执行真正的网络数据接收。EAGAIN 在这个场景下毫无意义——它既不能让出 CPU，也不能推进数据到达。

因此 `read_packet` 回调必须**阻塞等待数据到达后再返回**——这正是后续引入协程的动机。

## 多路流的传统方案与困境

当需要同时处理 N 路流时，由于每一路的 `read_packet` 都可能阻塞，传统方案是为每路流分配一个线程。每个线程独立运行 `av_read_frame` 循环，某一路阻塞时操作系统调度器运行其他线程。

这个方案能工作，但存在固有开销：

- **线程资源**：N 路流 = N 个线程，每个线程有独立的内核栈和默认 1MB～8MB 的用户栈
- **上下文切换**：线程间切换涉及内核态转换、TLB 刷新等系统开销
- **同步复杂度**：线程间共享数据需要 mutex、condition_variable 等同步原语

对于几十路流这些开销可以忽略，但在高密度场景下，多线程方案的扩展性就成为瓶颈。

更根本的问题是：每个线程大部分时间都在**阻塞等待 I/O**，真正做 demux/remux 计算的 CPU 时间很短。用重量级的操作系统线程来承载这种 I/O 密集型任务，是一种资源浪费。

## 协程：有栈与无栈

协程（Coroutine）是一种可以**主动挂起和恢复**的执行体。与线程由操作系统内核调度不同，协程的挂起和恢复完全在用户态完成，切换成本极低（只需保存/恢复少量寄存器），挂起时机由协程自身决定（协作式）。

多个协程可以运行在同一个线程上，通过**主动让出**（yield）来实现并发——当一个协程在等待 I/O 时 yield，事件循环就运行其他协程，直到 I/O 就绪后恢复原协程继续执行。

协程有两种主流实现方式，区别在于**如何保存挂起点的执行上下文**：

### 无栈协程（Stackless Coroutine）

代表：C++20 `co_await`、Rust `async/await`、Python `async/await`。

编译器把协程函数编译成一个状态机，每个 `co_await` / `co_yield` 点对应一个状态，局部变量被提取到堆上分配的"协程帧"（coroutine frame）中。挂起和恢复通过切换状态机的状态来实现。

关键限制：**只能在协程函数自身的 `co_await` 表达式处挂起**。如果协程调用了一个普通函数 A，A 又调用了 B，你不能在 B 的内部挂起这个协程——因为 A 和 B 的栈帧不在协程的状态机里。这就是"无栈"的含义：协程不拥有独立的调用栈，无法保存中间函数的执行状态。

### 有栈协程（Stackful Coroutine）

代表：Boost.Coroutine / Boost.Asio `spawn`、Go goroutine、Lua coroutine、State Threads。

有栈协程为每个协程分配一块独立的栈空间（通常几十 KB 到几百 KB）。挂起时保存当前的栈指针和寄存器，恢复时切回来——就像操作系统做线程上下文切换一样，只不过完全在用户态完成。

关键优势：**可以在任意调用深度处挂起**。哪怕当前执行到了 A → B → C → D 五层调用深处，协程照样可以挂起，因为整个调用栈都保存在协程自己的栈空间里。

## 核心洞察：有栈协程天然兼容同步回调式 C API

回顾前面的约束：

1. `read_packet` 是一个同步 C 回调函数，FFmpeg 在深层调用栈中调用它
2. `read_packet` 不能返回 EAGAIN——即使能，FFmpeg 也只是立即重试，不会把控制权交还给事件循环
3. 我们希望在没有数据时让出 CPU 给其他路流

如果使用**无栈协程**（C++20 `co_await`），问题在于 `read_packet` 是 FFmpeg 的 C 代码调用的普通函数——你无法在里面写 `co_await`。`co_await` 只能出现在协程函数体内，而不能出现在被 FFmpeg 内部调用的 C 回调里。这意味着 `read_packet` 内无法挂起协程，只能选择阻塞线程或返回错误码——又回到了老问题。

而**有栈协程**没有这个限制。有栈协程拥有自己的完整调用栈，挂起操作保存整个栈上下文，不关心当前在哪一层函数调用中。关键的执行流如下：

```
协程栈（由 spawn 分配的独立栈空间）
│
├─ 协程入口函数(yield_context yield)
│    └─ av_read_frame(ifmt_ctx, pkt)
│         └─ mpegts_read_packet()          ← FFmpeg 内部 C 代码
│              └─ fill_buffer()             ← FFmpeg 内部 C 代码
│                   └─ read_packet()        ← 我们的回调被 FFmpeg 调用
│                        │
│                        ├─ 缓冲区有数据 → 直接返回，FFmpeg 继续解析
│                        │
│                        └─ 缓冲区为空：
│                             └─ async_receive_from(..., yield[ec])
│                                  │
│                                  ╠══ 挂起：保存整个调用栈（包括 FFmpeg 的所有栈帧）
│                                  ║        事件循环运行其他协程
│                                  ║        ...
│                                  ║        网络数据到达
│                                  ╠══ 恢复：从这里继续执行
│                                  │
│                                  └─ 返回收到的数据
│                                       └─ read_packet 返回给 fill_buffer
│                                            └─ FFmpeg 继续解析
│                                                 （完全不知道中间发生过挂起/恢复）
```

FFmpeg 内部的整条调用链——`mpegts_read_packet` → `fill_buffer` → `read_packet`——全部运行在协程的栈上。挂起时整个栈被冻结，恢复时从异步 I/O 返回处继续执行。**FFmpeg 完全不知道中间发生过一次挂起/恢复**，从它的视角看 `read_packet` 就是一个普通的同步函数调用，进去、拿到数据、回来。

这就是核心洞察：**有栈协程保存的是整个调用栈，所以它天然兼容同步回调式的 C API**。只要 C 库的回调函数运行在协程的栈上，协程就可以在回调内部透明地挂起和恢复，而 C 库完全不需要做任何修改。

## 实现要点

基于这个原理，实现时需要关注几个要点：

### 数据缓冲区设计

`read_packet` 回调需要一个中间缓冲区来暂存从网络收到的数据。缓冲区的核心逻辑是：

- FFmpeg 请求数据时，如果缓冲区有数据，直接消费返回
- 如果缓冲区为空，通过异步 I/O 操作 yield 等待数据到达，数据到达后填充缓冲区再返回

缓冲区对象作为 `opaque` 参数传入 AVIO context，同时持有 `yield_context` 的引用，使得在回调内部可以执行异步操作。

### 从 probing 阶段开始使用同一套回调

FFmpeg 在 `avformat_open_input` 和 `avformat_find_stream_info` 阶段会频繁调用 `read_packet` 来分析输入格式。这个 probing 阶段同样需要从网络获取数据。

由于有栈协程的 yield 机制从协程启动时就可用，probing 阶段和后续的主循环可以使用**完全相同的回调函数和 AVIO context**。不需要区分"预填充模式"和"正常模式"，不需要在两个阶段之间切换 I/O 上下文。这简化了实现，也避免了 FFmpeg 内部状态不一致的风险。

### 写出方向同理

输出侧的 `write_packet` 回调同样可以利用协程。FFmpeg 封装好的数据通过回调写入缓冲区，然后协程通过异步 I/O 操作将数据发送出去。发送时同样 yield，让出 CPU 给其他协程。

### 调度模型

所有协程注册到同一个事件循环（如 Boost.Asio 的 `io_context`），在单个线程上协作式调度。事件循环通过操作系统的 I/O 多路复用机制（epoll / kqueue）高效地管理所有协程的 I/O 就绪事件。

```
io_context.run()  ─── 单线程事件循环
    │
    ├─ 协程 A 就绪 → 运行 → yield（等 UDP 数据）→ 挂起
    ├─ 协程 B 就绪 → 运行 → yield（等 UDP 数据）→ 挂起
    ├─ 协程 A 的数据到达 → 恢复 → 运行 → yield → 挂起
    ├─ 协程 C 就绪 → 运行 → ...
    └─ ...
```

## 方案对比

| 方案 | 线程数 | 同步原语 | 可在 read_packet 内挂起 | 扩展性 |
|------|--------|---------|----------------------|--------|
| 多线程 | N 路 = N 线程 | mutex + condvar | 不需要（每线程阻塞） | 受线程数限制 |
| 无栈协程 (C++20) | 需要 demux 线程 + I/O 线程 | mutex + condvar | ❌ 不能 | 受限于线程间同步 |
| **有栈协程** | **1 线程** | **无** | **✅ 可以** | **受限于单线程 CPU** |

## 结语

FFmpeg 的 AVIO 回调是同步阻塞式的 C 函数指针设计。这个设计本身没有问题——它是最通用、最简洁的 I/O 抽象。但它确实给单线程并发处理多路流带来了挑战。

有栈协程恰好能化解这个矛盾：它可以在**任意调用深度**处挂起执行，包括第三方 C 库深层调用栈中的回调函数内部。这使得我们可以在单线程内，用纯协作式调度处理多路流——不需要额外线程、不需要锁、不需要条件变量，FFmpeg 的调用方式也完全不变。

这个模式不仅适用于 FFmpeg，也适用于任何基于同步回调的 C 库：只要库的回调函数运行在有栈协程的栈上，协程就可以在回调内部透明地挂起和恢复，而库完全不需要知道这件事。
