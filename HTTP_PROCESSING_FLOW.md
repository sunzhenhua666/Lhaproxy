# HAProxy HTTP 处理入口和数据流程

## 一、HTTP 处理入口点

虽然 `mux_http_ops` 结构体中没有直接的 HTTP 处理入口函数，但 HTTP 处理是通过以下入口点启动的：

### 1. 连接接受入口
**文件**: `src/session.c`

```c
session_accept_fd(struct connection *cli_conn)
```
- **作用**: 接受新的客户端连接
- **流程**:
  1. 创建 session (`session_new()`)
  2. 执行 TCP 规则 (`tcp_exec_l4_rules()`)
  3. 启动传输层 (`conn_xprt_start()`)
  4. 调用 `conn_complete_session()` 完成会话初始化

### 2. 会话完成入口
**文件**: `src/session.c:523`

```c
conn_complete_session(struct connection *conn)
```
- **作用**: 完成会话创建，安装 mux
- **关键步骤**:
  - 执行 TCP 会话规则 (`tcp_exec_l5_rules()`)
  - 调用 `conn_install_mux_fe()` 安装前端 mux

### 3. Mux 安装入口
**文件**: `src/connection.c:265`

```c
conn_install_mux_fe(struct connection *conn, void *ctx)
```
- **作用**: 为前端连接安装合适的 mux
- **流程**:
  1. 根据 frontend 模式（HTTP/TCP）和 ALPN 选择 mux
  2. 调用 `conn_install_mux()` 安装 mux
  3. 调用 `mux->init()` (即 `h1_init()`) 初始化 H1 mux

**文件**: `include/haproxy/connection.h:486`

```c
static inline int conn_install_mux(struct connection *conn, 
                                   const struct mux_ops *mux,
                                   void *ctx, struct proxy *prx, 
                                   struct session *sess)
```
- **作用**: 实际安装 mux，设置 `conn->mux` 和 `conn->ctx`
- **关键**: 调用 `mux->init()` 初始化 mux 上下文

## 二、HTTP 数据流程

### 1. 接收数据流程（从客户端到应用层）

```
网络层接收数据
    ↓
传输层 (xprt) 读取到连接缓冲区
    ↓
stconn.c:sc_applet_process() / sc_conn_io_cb()
    ↓
conn->mux->rcv_buf()  [mux_h1.c:4288]
    ↓
h1_rcv_buf()
    ↓
h1_process_demux()  [mux_h1.c:1768]
    ↓
解析 HTTP 消息:
  - h1_handle_headers()  - 解析请求/响应头
  - h1_handle_data()     - 解析请求/响应体
  - h1_handle_trailers() - 解析尾部
    ↓
转换为 HTX 格式，写入 channel 缓冲区
    ↓
应用层 (stream) 处理
```

**关键函数**:
- `src/stconn.c:1377`: `conn->mux->rcv_buf(sc, &ic->buf, max, cur_flags)`
- `src/mux_h1.c:4288`: `h1_rcv_buf()` - mux 接收入口
- `src/mux_h1.c:1768`: `h1_process_demux()` - HTTP/1 解复用处理
  - `h1_handle_headers()` - 解析 HTTP 头部
  - `h1_handle_data()` - 解析 HTTP 数据体
  - `h1_handle_trailers()` - 解析 HTTP 尾部

### 2. 发送数据流程（从应用层到客户端）

```
应用层 (stream) 生成响应
    ↓
写入 channel 缓冲区 (HTX 格式)
    ↓
stconn.c:sc_applet_process() / sc_conn_io_cb()
    ↓
conn->mux->snd_buf()  [mux_h1.c:4324]
    ↓
h1_snd_buf()
    ↓
h1_process_mux()  [mux_h1.c:3064]
    ↓
格式化 HTTP 消息:
  - h1_make_reqline() / h1_make_stline() - 生成请求/状态行
  - h1_make_headers()  - 生成 HTTP 头部
  - h1_make_data()     - 生成 HTTP 数据体
  - h1_make_trailers() - 生成 HTTP 尾部
    ↓
写入连接输出缓冲区 (h1c->obuf)
    ↓
h1_send() 发送到网络层
    ↓
传输层 (xprt) 写入 socket
```

**关键函数**:
- `src/stconn.c:1695`: `conn->mux->snd_buf(sc, &oc->buf, co_data(oc), send_flag)`
- `src/mux_h1.c:4324`: `h1_snd_buf()` - mux 发送入口
- `src/mux_h1.c:3064`: `h1_process_mux()` - HTTP/1 复用处理
  - `h1_make_reqline()` / `h1_make_stline()` - 生成请求/状态行
  - `h1_make_headers()` - 生成 HTTP 头部
  - `h1_make_data()` - 生成 HTTP 数据体
  - `h1_make_trailers()` - 生成 HTTP 尾部

## 三、关键数据结构

### 1. mux_ops 结构体
**位置**: `src/mux_h1.c:5304`

```c
static const struct mux_ops mux_http_ops = {
    .init        = h1_init,           // 初始化 mux
    .wake        = h1_wake,            // 唤醒 mux
    .attach      = h1_attach,          // 附加 stream
    .rcv_buf     = h1_rcv_buf,         // 接收数据入口 ⭐
    .snd_buf     = h1_snd_buf,         // 发送数据入口 ⭐
    .detach      = h1_detach,          // 分离 stream
    .destroy     = h1_destroy,         // 销毁 mux
    // ... 其他操作
};
```

### 2. H1 连接结构 (h1c)
**位置**: `src/mux_h1.c:37`

```c
struct h1c {
    struct connection *conn;      // 底层连接
    struct h1s *h1s;              // H1 stream
    struct buffer ibuf;            // 输入缓冲区
    struct buffer obuf;             // 输出缓冲区
    enum h1_cs state;              // 连接状态
    // ...
};
```

### 3. H1 Stream 结构 (h1s)
**位置**: `src/mux_h1.c:64`

```c
struct h1s {
    struct h1c *h1c;               // 所属 H1 连接
    struct sedesc *sd;             // stream connector 描述符
    struct session *sess;          // 关联的 session
    struct h1m req;                // HTTP 请求消息
    struct h1m res;                // HTTP 响应消息
    // ...
};
```

## 四、数据流转示意图

```
┌─────────────┐
│   Client    │
└──────┬──────┘
       │ HTTP Request
       ↓
┌─────────────────────────────────────────┐
│         Network Layer (Socket)          │
└──────┬──────────────────────────────────┘
       │
       ↓
┌─────────────────────────────────────────┐
│    Transport Layer (xprt: TCP/SSL)      │
│    - conn_xprt_start()                   │
│    - xprt->rcv_pipe() / snd_pipe()      │
└──────┬──────────────────────────────────┘
       │
       ↓
┌─────────────────────────────────────────┐
│      Stream Connector (stconn)          │
│    - sc_applet_process()                │
│    - sc_conn_io_cb()                    │
└──────┬──────────────────────────────────┘
       │
       ↓
┌─────────────────────────────────────────┐
│         Mux Layer (mux_h1)              │
│    - h1_rcv_buf() / h1_snd_buf()       │
│    - h1_process_demux() / h1_process_mux()│
│    - HTTP/1 解析/格式化                  │
└──────┬──────────────────────────────────┘
       │ HTX Format
       ↓
┌─────────────────────────────────────────┐
│      Application Layer (stream)         │
│    - HTTP 规则处理                       │
│    - 负载均衡                            │
│    - 后端连接                            │
└─────────────────────────────────────────┘
```

## 五、总结

1. **HTTP 处理入口**: 
   - 虽然 `mux_http_ops` 中没有直接的 HTTP 处理函数，但通过 `rcv_buf` 和 `snd_buf` 两个接口函数实现数据接收和发送
   - 真正的 HTTP 解析在 `h1_process_demux()` 和 `h1_process_mux()` 中完成

2. **数据流程**:
   - **接收**: 网络层 → 传输层 → stconn → mux->rcv_buf() → h1_process_demux() → HTX → stream
   - **发送**: stream → HTX → mux->snd_buf() → h1_process_mux() → stconn → 传输层 → 网络层

3. **关键点**:
   - `h1_rcv_buf()` 和 `h1_snd_buf()` 是 mux 层与应用层的接口
   - `h1_process_demux()` 负责将原始 HTTP/1 数据解析为 HTX 格式
   - `h1_process_mux()` 负责将 HTX 格式数据格式化为 HTTP/1 协议数据
