# 安全策略

## 支持的版本

| 版本 | 安全修复 |
|------|----------|
| 2.0.x | ✅ |
| < 2.0 | ❌ |

## 报告漏洞

**请不要用公开 issue 报告安全问题**，那会让所有部署者在你修复前暴露。

优先使用 GitHub 私密漏洞报告：本仓库 **Security → Advisories → Report a
vulnerability**。也可以邮件联系 `service@nodemedia.cn`。

报告时请尽量包含：

- 受影响的版本或 commit
- 复现步骤或最小 PoC（可复现的推流/请求序列、构造的流样本）
- 影响评估（崩溃 / 越界 / 拒绝服务 / 规则绕过）
- 是否愿意在修复公告中具名致谢

## 重点关注范围

- RTMP 与 HTTP-FLV 解析路径的内存安全：越界读/写、整数溢出、use-after-free
- 由畸形流、超长字段或大量并发连接造成的拒绝服务
- `allow/deny publish|play` 访问规则被绕过
- 与 nginx 交互时的资源泄漏（连接、缓冲、共享缓冲引用计数）

## 响应方式

维护者会尽力在 7 天内确认收到、30 天内给出修复或缓解方案。修复随补丁版本
发布，并在 Release 说明与 GitHub Security Advisory 中致谢（除非报告者要求匿名）。

## 部署建议

- 不要将 `live_flv` 的 POST 推流端点直接暴露在公网；用 `allow/deny` 或反代鉴权限制来源
- 关注 `logs/error.log` 中的异常流告警
