# nginx-live-module

[简体中文](README_CN.md) | **English**

A modern live-streaming module for nginx: one module handles **RTMP**, **HTTP-FLV** and **WebSocket-FLV** publishing/playback. Written in pure C and compiled into nginx statically or dynamically.

![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-lightgrey.svg)
![nginx](https://img.shields.io/badge/nginx-1.31.x-green.svg)
![Language](https://img.shields.io/badge/language-C99-orange.svg)

## Why use it

- **No second service to run**: the module is compiled into the nginx you already run — no FFmpeg remuxing, no separate media-server process
- **One publisher, many viewers, zero copy**: a single stream is played simultaneously over RTMP, HTTP-FLV and WebSocket-FLV; reference-counted shared buffers mean 1 publish to N pulls requires no amplified copying
- **Instant playback out of the box**: keyframe-aligned GOP cache gets the first frame to HTTP-FLV players in ≤ 1 GOP
- **Built for modern codecs**: Enhanced RTMP v1 supports AV1 / VP9 / HEVC (video) and Opus / AC-3 / EAC-3 (audio)
- **Configuration compatible with nginx-rtmp-module**: the `rtmp { server { application { } } }` structure is identical, so migration only requires changing the HTTP-side directive prefix

## Download (prebuilt Windows x64 package)

Don't want to build it yourself? [**nginx-live-win32**](https://github.com/illuspas/nginx-live-win32) is the official Windows binary release repository:

- `git clone` or download the ZIP for a portable package — unzip and run (`nginx.exe` + `conf` + `html`)
- Ships the **nginx-live Service Manager** (`nginx_service.exe`): install as a Windows service in one click, start on boot, manage from the tray, open config/page
- The bundled `BUILD-INFO.txt` records the nginx/module versions, build flags and SHA256 of both executables for that release; `LICENSES/` contains the full license texts shipped with the package

> The package is a combined binary of nginx + this module + OpenSSL/PCRE2/zlib; see `THIRD-PARTY-NOTICES.md` inside the package for each component's copyright and license.

## Quick start (from source)

```bash
git clone https://github.com/nginx/nginx.git && cd nginx

# Static module
auto/configure --add-module=/path/to/nginx-live-module
make -j && make install

# Or a dynamic module (load into an existing ABI-compatible nginx, no nginx rebuild)
auto/configure --add-dynamic-module=/path/to/nginx-live-module
make -j
```

Minimal configuration:

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

Run it:

```bash
./objs/nginx -p nginx-live-module/test/ -c nginx.conf

# RTMP publish (in another terminal)
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/myapp/test

# RTMP playback
ffplay rtmp://127.0.0.1:1935/myapp/test

# HTTP-FLV playback (same stream)
ffplay http://127.0.0.1:8080/myapp/test.flv

# WebSocket-FLV publish (FFmpeg 8.0+ has native ws support; builds with the subprotocol built in may omit -subprotocol)
ffmpeg -re -i input.mp4 -c copy -f flv -subprotocol post ws://127.0.0.1:8080/myapp/test.flv

# WebSocket-FLV playback (flv.js / NodePlayer work in the browser)
ffplay ws://127.0.0.1:8080/myapp/test.flv

# Status
curl -s http://127.0.0.1:8080/stat | jq .
```

## Differences from nginx-rtmp-module

| | nginx-live-module | nginx-rtmp-module |
|---|---|---|
| RTMP publish/play | ✅ | ✅ |
| HTTP-FLV publish (`POST /app/name.flv`) | ✅ | ❌ |
| HTTP-FLV playback (`GET /app/name.flv`) | ✅ | ❌ |
| WebSocket-FLV publish (subprotocol `post`, same contract as NMS) | ✅ | ❌ |
| WebSocket-FLV playback (`ws://` / `wss://`, same port and location) | ✅ | ❌ |
| Enhanced RTMP v1 (AV1 / VP9 / HEVC / Opus / AC-3 / EAC-3) | ✅ | ❌ |
| Zero-copy fan-out (reference-counted shared buffers) | ✅ | ❌ |
| Access control shared across protocols (`allow/deny publish\|play`) | ✅ | Partial |
| Status endpoint | `live_stat` (JSON) | `rtmp_stat` (XML/HTML) |
| Configuration structure | `rtmp { server { application { } } }` (compatible) | Same |

Migration notes: HTTP-side directives keep the `live_` prefix (`live_flv` / `live_stat`, with no `rtmp_stat` / `rtmp_control` aliases);
**it cannot be compiled together with `nginx-rtmp-module`** — both register the top-level `rtmp{}` block directive, so they conflict.

## Features

- RTMP: SimpleHandshake, chunk protocol (inbound dynamic chunk size, extended timestamps, all fmt0-3 formats),
  AMF0 commands, full publish/play flow; interoperates with FFmpeg / Node-Media-Server / nginx-rtmp-module
- HTTP-FLV: publish with `POST /app/name` (chunked / Content-Length / 100-continue),
  play with `GET /app/name.flv` (chunked, instant start in ≤ 1 GOP)
- WebSocket-FLV: same port and location as HTTP-FLV, per-request upgrade (RFC 6455). Direction is determined by the handshake
  subprotocol — `Sec-WebSocket-Protocol: post` (or `publisher`) means publish, otherwise playback; the wire bytes are exactly the same as HTTP-FLV,
  only framed into WebSocket binary frames at tag granularity, so flv.js / NodePlayer / ffplay can play it directly. `wss://` is provided directly by `listen ... ssl`
- Zero-copy fan-out: reference-counted shared buffers, 1 publish to N pulls with no amplified copying
- GOP cache: keyframe-aligned instant start, whole-GOP caching with a packet-count cap as a safeguard, inherited level by level
- Backpressure: slow subscribers drop frames and resync / are disconnected, without affecting the publisher or other subscribers
- Access control: `allow/deny publish|play` inside the `rtmp{}` block (rtmp/server/application contexts, protocol-agnostic, shared by RTMP/HTTP)
- Status endpoint: `live_stat on` emits JSON stream statistics
- Pure C implementation, compiled as an nginx third-party module (static or dynamic), no external processes

## Directives

| Directive | Context | Default | Description |
|-----------|---------|---------|-------------|
| `rtmp {}` | main | — | Live configuration block (CORE module, structure compatible with nginx-rtmp-module) |
| `server {}` | rtmp | — | Server block |
| `listen <addr:port> [bind\|ipv6only=on\|off]` | server | — | RTMP listen port (may be repeated) |
| `application <name> {}` | server | — | Defines an application |
| `live on\|off` | rtmp, server, application | off | Publish/play switch (off returns 503 / rejects) |
| `gop_cache on\|off` | rtmp, server, application | on | GOP cache for instant start (whole-GOP caching, 4096-packet safeguard cap), inherited level by level |
| `chunk_size <n>` | rtmp, server | 65535 | RTMP outbound chunk size (1024..16777215) |
| `timeout <t>` | rtmp, server | 60s | Handshake/send/publish idle timeout |
| `max_streams <n>` | rtmp, server | 64 | RTMP chunk stream ID limit |
| `ack_window <n>` | rtmp, server | 5000000 | WindowAckSize advertised to peers |
| `max_message <size>` | rtmp, server | 4m | RTMP message reassembly limit |
| `allow [publish\|play] <cidr>\|all` | rtmp, server, application | — | Allow rule (matched in order, allow by default) |
| `deny [publish\|play] <cidr>\|all` | rtmp, server, application | — | Deny rule (returns 403) |
| `live_flv on\|off` | http, server, location | off | HTTP-FLV publish/play endpoint |
| `live_stat on\|off` | http, server, location | off | JSON status endpoint |

## What production deployments also need

This module covers the **protocol and forwarding layer**: single machine, in-process, no external dependencies. The following capabilities are outside the scope of the open-source module
and are provided by **NodeMediaServer V3** (a commercial product) — if you need one of them, using it directly is simpler:

| Capability | nginx-live-module (Apache-2.0) | NodeMediaServer V3 (commercial) |
|------------|--------------------------------|---------------------------------|
| Protocols | RTMP, HTTP-FLV | Also SRT, WebRTC, kmp ultra-low latency |
| Deployment | Single-machine nginx module | Edge clusters, managed cloud, horizontal scaling |
| Security & auth | `allow/deny` CIDR rules | Publish/play authentication, token/callback, hotlink protection |
| Media processing | Forwarding only (no transcoding, no recording) | Transcoding, recording, snapshots, watermarking, stream mixing |
| Observability | `/stat` JSON, error.log | Monitoring and alerting, REST API, operational reports |
| Clients | Any RTMP/FLV player | Cross-platform player SDK + publisher SDK |
| Support | Community (GitHub Issues) | Commercial SLA, technical support, custom development |

- Website and docs: <https://www.nodemedia.cn>
- Sales and technical support: `service@nodemedia.cn`
- Related open-source project: [Node-Media-Server](https://github.com/illuspas/Node-Media-Server) (Node.js RTMP/HTTP-FLV server, Apache-2.0)

## Build

From the nginx source root:

```bash
auto/configure --add-module=/path/to/nginx-live-module
make
```

For Windows (MSVC + service manager + official binary packaging), use `msvc-build\build.cmd` in the `nginx-live` repository;
for MinGW cross-compilation see [mingw-cross-build.md](mingw-cross-build.md).

## Status and testing

```bash
curl -s http://127.0.0.1:8080/stat | jq .
```

`test/` contains ready-to-run scripts: `push.sh` (publish), `play.sh` (playback), `rtmp.sh` (full RTMP flow),
`matrix.sh` (combination matrix), `load.sh` (load). They require `ffmpeg` / `ffplay` on the system.

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute
- [SECURITY.md](SECURITY.md) — how to report vulnerabilities

## License

[Apache License 2.0](LICENSE). Third-party components and attribution are declared in [NOTICE](NOTICE).

nginx is a registered trademark of Nginx, Inc.; this project is an independent third-party module and is not affiliated with,
sponsored by, or endorsed by Nginx, Inc.
