# nginx-live-module

现代化 nginx 直播流媒体模块：一个模块搞定 **RTMP**、**HTTP-FLV** 与 **WebSocket-FLV** 推流/播放，纯 C 实现，静态或动态编译进 nginx。

![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-lightgrey.svg)
![nginx](https://img.shields.io/badge/nginx-1.31.x-green.svg)
![Language](https://img.shields.io/badge/language-C99-orange.svg)

## 为什么用它

- **不用装第二套服务**：模块编进你已经在跑的 nginx，不需要 FFmpeg 转封装、不需要额外的媒体服务器进程
- **一推多出、零拷贝**：同一路推流同时被 RTMP、HTTP-FLV 与 WebSocket-FLV 播放，引用计数共享缓冲，1 推 N 拉不做放大拷贝
- **开箱秒开**：GOP cache 关键帧对齐，HTTP-FLV 播放端首帧 ≤ 1 GOP
- **面向现代编码器**：Enhanced RTMP v1 支持 AV1 / VP9 / HEVC（视频）与 Opus / AC-3 / EAC-3（音频）
- **配置与 nginx-rtmp-module 兼容**：`rtmp { server { application { } } }` 结构照搬，迁移只改 HTTP 侧指令前缀

## 下载（Windows x64 预编译包）

不想自己编译？[**nginx-live-win32**](https://github.com/illuspas/nginx-live-win32) 是官方 Windows 二进制发布仓库：

- `git clone` 或直接下载 ZIP 即为绿色包，解压即用（`nginx.exe` + `conf` + `html`）
- 内含 **nginx-live Service Manager**（`nginx_service.exe`）：一键安装为 Windows 服务、开机自启、托盘管理、打开配置/页面
- 包内 `BUILD-INFO.txt` 记录该版本的 nginx/模块版本、编译参数与两个 exe 的 SHA256；`LICENSES/` 为随包许可证全文

> 包内为 nginx + 本模块 + OpenSSL/PCRE2/zlib 的组合二进制，各组件版权与许可证见包内 `THIRD-PARTY-NOTICES.md`。

## 快速开始（源码）

```bash
git clone https://github.com/nginx/nginx.git && cd nginx

# 静态模块
auto/configure --add-module=/path/to/nginx-live-module
make -j && make install

# 或者动态模块（加载进已有的同 ABI nginx，无需重新编译 nginx）
auto/configure --add-dynamic-module=/path/to/nginx-live-module
make -j
```

最小配置：

```nginx
rtmp {
    server {
        listen 1935;
        application live {
            live on;
        }
    }
}

http {
    server {
        listen 8080;
        location ~ ^/[a-z0-9_]+/.+(\.flv)?$ { live_flv on; }
        location = /stat { live_stat on; }
    }
}
```

跑起来：

```bash
./objs/nginx -p nginx-live-module/test/ -c nginx.conf

# RTMP 推流（另开终端）
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/myapp/test

# RTMP 播放
ffplay rtmp://127.0.0.1:1935/myapp/test

# HTTP-FLV 播放（同一路流）
ffplay http://127.0.0.1:8080/myapp/test.flv

# WebSocket-FLV 推流（FFmpeg 8.0+ 原生 ws 协议；自编译内置子协议的版本可省略 -subprotocol）
ffmpeg -re -i input.mp4 -c copy -f flv -subprotocol post ws://127.0.0.1:8080/myapp/test.flv

# WebSocket-FLV 播放（浏览器端可用 flv.js / NodePlayer）
ffplay ws://127.0.0.1:8080/myapp/test.flv

# 状态
curl -s http://127.0.0.1:8080/stat | jq .
```

## 与 nginx-rtmp-module 的差异

| | nginx-live-module | nginx-rtmp-module |
|---|---|---|
| RTMP 推/拉 | ✅ | ✅ |
| HTTP-FLV 推流（`POST /app/name.flv`） | ✅ | ❌ |
| HTTP-FLV 拉流（`GET /app/name.flv`） | ✅ | ❌ |
| WebSocket-FLV 推流（子协议 `post`，NMS 同款契约） | ✅ | ❌ |
| WebSocket-FLV 拉流（`ws://` / `wss://`，同端口同 location） | ✅ | ❌ |
| Enhanced RTMP v1（AV1 / VP9 / HEVC / Opus / AC-3 / EAC-3） | ✅ | ❌ |
| 零拷贝 fan-out（引用计数共享缓冲） | ✅ | ❌ |
| 访问控制跨协议共用（`allow/deny publish\|play`） | ✅ | 部分 |
| 状态端点 | `live_stat`（JSON） | `rtmp_stat`（XML/HTML） |
| 配置结构 | `rtmp { server { application { } } }`（兼容） | 同 |

迁移注意：HTTP 侧指令保留 `live_` 前缀（`live_flv` / `live_stat`，无 `rtmp_stat` / `rtmp_control` 别名）；
**不能与 `nginx-rtmp-module` 同时编译**——两者都注册顶层 `rtmp{}` 块指令，会相互冲突。

## 特性

- RTMP：SimpleHandshake，chunk 协议（入站动态 chunk size、扩展时间戳、fmt0-3 全格式），
  AMF0 命令，publish/play 全流程；与 FFmpeg / Node-Media-Server / nginx-rtmp-module 互通
- HTTP-FLV：`POST /app/name` 推流（chunked / Content-Length / 100-continue），
  `GET /app/name.flv` 播放（chunked，秒开 ≤ 1 GOP）
- WebSocket-FLV：与 HTTP-FLV 同端口同 location、逐请求升级（RFC 6455）。方向由握手
  子协议判定——携带 `Sec-WebSocket-Protocol: post`（或 `publisher`）为推流，
  缺省为播放；线上字节与 HTTP-FLV 完全一致，仅按 tag 粒度封装 WS 二进制帧，
  flv.js / NodePlayer / ffplay 直接可播。`wss://` 由 `listen ... ssl` 直接提供
- 零拷贝 fan-out：引用计数共享缓冲，1 推 N 拉无放大拷贝
- GOP cache：关键帧对齐秒开，整 GOP 缓存 + 条数上限兜底，逐级继承
- 背压：慢速订阅者丢帧重同步 / 断开，不影响发布者与其他订阅者
- 访问控制：`rtmp{}` 块内 `allow/deny publish|play`（rtmp/server/application 语境，协议无关，RTMP/HTTP 共用）
- 状态端点：`live_stat on` 输出 JSON 流统计
- 纯 C 实现，作为 nginx 第三方模块静态/动态编译，无需外部进程

## 指令一览

| 指令 | 语境 | 默认 | 说明 |
|------|------|------|------|
| `rtmp {}` | main | — | 直播配置块（CORE 模块，结构与 nginx-rtmp-module 兼容） |
| `server {}` | rtmp | — | 服务器块 |
| `listen <addr:port> [bind\|ipv6only=on\|off]` | server | — | RTMP 监听端口（可多条） |
| `application <name> {}` | server | — | 定义应用 |
| `live on\|off` | rtmp, server, application | off | 推拉开关（off 返回 503 / 拒绝） |
| `gop_cache on\|off` | rtmp, server, application | on | GOP 缓存秒开（整 GOP 缓存，4096 包防护上限），逐级继承 |
| `chunk_size <n>` | rtmp, server | 65535 | RTMP 出站 chunk size（1024..16777215） |
| `timeout <t>` | rtmp, server | 60s | 握手/发送/发布空闲超时 |
| `max_streams <n>` | rtmp, server | 64 | RTMP chunk stream id 上限 |
| `ack_window <n>` | rtmp, server | 5000000 | 通告给对端的 WindowAckSize |
| `max_message <size>` | rtmp, server | 4m | RTMP 消息重组上限 |
| `allow [publish\|play] <cidr>\|all` | rtmp, server, application | — | 放行规则（按序匹配，缺省放行） |
| `deny [publish\|play] <cidr>\|all` | rtmp, server, application | — | 拒绝规则（返回 403） |
| `live_flv on\|off` | http, server, location | off | HTTP-FLV 推/拉端点 |
| `live_stat on\|off` | http, server, location | off | JSON 状态端点 |

## 生产环境还需要什么

本模块解决的是**协议与转发层**：单机、进程内、无外部依赖。以下能力不在开源模块范围内，
由 **NodeMediaServer V3**（商业产品）提供，如果你需要其中的一项，直接用它更省事：

| 能力 | nginx-live-module（Apache-2.0） | NodeMediaServer V3（商业） |
|------|--------------------------------|----------------------|
| 协议 | RTMP、HTTP-FLV | 另含 SRT、WebRTC、kmp 超低延迟 |
| 部署形态 | 单机 nginx 模块 | 边缘集群、云托管、水平扩容 |
| 安全与鉴权 | `allow/deny` CIDR 规则 | 推拉流鉴权、Token/回调、防盗链 |
| 媒体处理 | 只做转发（不转码、不录制） | 转码、录制、截图、水印、混流 |
| 可观测性 | `/stat` JSON、error.log | 监控告警、REST API、运营报表 |
| 客户端 | 任意 RTMP/FLV 播放器 | 全平台播放器 SDK + 推流 SDK |
| 支持 | 社区（GitHub Issues） | 商业 SLA、技术支持、定制开发 |

- 官网与文档：<https://www.nodemedia.cn>
- 商务与技术支持：`service@nodemedia.cn`
- 相关开源项目：[Node-Media-Server](https://github.com/illuspas/Node-Media-Server)（Node.js 版 RTMP/HTTP-FLV 服务端，Apache-2.0）

## 构建

在 nginx 源码根目录执行：

```bash
auto/configure --add-module=/path/to/nginx-live-module
make
```

Windows（MSVC + 服务管理器 + 官方二进制打包）请用 `nginx-live` 仓库的 `msvc-build\build.cmd`；
MinGW 交叉编译见 [mingw-cross-build.md](mingw-cross-build.md)。

## 状态与测试

```bash
curl -s http://127.0.0.1:8080/stat | jq .
```

`test/` 下有可直接运行的脚本：`push.sh`（推流）、`play.sh`（播放）、`rtmp.sh`（RTMP 全流程）、
`matrix.sh`（组合矩阵）、`load.sh`（负载）。需要系统中已有 `ffmpeg` / `ffplay`。

## 文档

- [CONTRIBUTING.md](CONTRIBUTING.md) — 如何参与开发
- [SECURITY.md](SECURITY.md) — 漏洞报告方式

## 许可

[Apache License 2.0](LICENSE)。第三方组件与来源声明见 [NOTICE](NOTICE)。

nginx 是 Nginx, Inc. 的注册商标；本项目是独立第三方模块，与 Nginx, Inc.
不存在隶属、赞助或背书关系。
