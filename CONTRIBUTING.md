# 贡献指南

## 许可与贡献授权

本项目采用 **Apache License 2.0**。提交 Pull Request 即表示你同意你的贡献按
Apache-2.0 授权（inbound = outbound），并确认你有权这样做。

若某处改动同时会被用于闭源的商业产品（NodeMedia SDK），我们可能会另行请你
签署一份 CLA；在签署之前，该改动只会进入本开源模块。

## 开发环境

### Linux / macOS

```bash
git clone https://github.com/nginx/nginx.git
cd nginx
auto/configure --add-module=/path/to/nginx-live-module    # 静态模块
make -j

# 动态模块（可加载进已有的同 ABI nginx）
auto/configure --add-dynamic-module=/path/to/nginx-live-module
make -j
```

### Windows

本仓库只包含模块源码。MSVC 构建脚本、服务管理器与官方二进制打包流程在
独立仓库 `nginx-live`（`msvc-build\build.cmd`、`nginx-service-win32\`、
`publish.cmd`）中。也可以参考 [mingw-cross-build.md](mingw-cross-build.md)
用 MinGW 交叉编译。

## 代码风格

- 遵循 nginx 源码风格：4 空格缩进、`ngx_` 前缀、`ngx_int_t` / `ngx_str_t` /
  `ngx_array_t` 等既有类型；错误返回用 `NGX_OK` / `NGX_ERROR` / `NGX_DECLINED`
- 日志统一走 `ngx_log_error()`，不要用 `printf`
- 源文件保持**纯 ASCII**（注释写英文）：避免不同编译器代码页差异（MSVC `/utf-8`
  与 MinGW/GBK 环境混用时容易出问题）
- 新增源文件/头文件必须在 `config` 中登记（源码位于 `src/`，路径用
  `$ngx_addon_dir/src/...`）；`config` 必须留在仓库根目录，nginx 的
  `--add-module` 只识别该位置
- 分配内存用 nginx 池（`ngx_palloc` 等），不要在请求/连接路径上用 `malloc`

## 提交信息

沿用仓库既有约定（中文 Conventional Commits）：

```
feat: 新增 xxx 指令
fix: 修复 xxx 场景下的越界读
docs: 补充 xxx 说明
refactor: 拆分 xxx
test: 增加 xxx 用例
chore: 调整构建脚本
```

## 验证要求

提 PR 前请自行验证：

1. 编译通过：静态模块与 `--add-dynamic-module` 各一次，无新增警告
2. 跑一遍 `test/` 下相关脚本（需要 `ffmpeg` / `ffplay`）：

   ```bash
   ./objs/nginx -p nginx-live-module/test/ -c nginx.conf
   bash nginx-live-module/test/rtmp.sh
   curl -s http://127.0.0.1:8080/stat | jq .
   ```

3. PR 描述里说明：改了哪条指令或协议行为、如何复现验证、贴出关键命令与输出

## 报告问题

- 普通 bug / 功能请求：GitHub Issues
- 安全漏洞：**不要**开公开 issue，见 [SECURITY.md](SECURITY.md)
