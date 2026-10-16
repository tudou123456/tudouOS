#ifndef TUDOU_MCP_H
#define TUDOU_MCP_H

/*
 * MCP 协议适配器（M6，支柱1 收官；D-20 语义对齐 2025-06-18 规范核心）
 * ——见 docs/mcp-bus-design.md。
 *
 * 传输绑定（当前：UART 行传输）：控制台识别 '{' 开头的行投递到
 * 消息基座通道；代理任务从通道取行 → JSON-RPC 解析 → 工具分发 →
 * 响应单行 JSON 回传。实时语义（有界/去重/回执）由消息基座提供。
 *
 * JSON-RPC/MCP 语义：initialize / notifications/initialized 握手、
 * tools/list（{name,description,inputSchema} 数组）、tools/call
 * （{name,arguments} → {content,isError}）；id 三态原样回显（数字/
 * 字符串），无 id 即 notification 不回包；兼容旧直连形态（method
 * 写工具名，params 即参数）。
 * 已知边界：串口与控制台共享，流里混有回显与提示符，严格客户端需
 * 过滤非 JSON 行（独占传输/帧标识留待后续里程碑）。
 */

void mcp_init(void);          /* 通道初始化（内核启动期） */
int  mcp_start(void);         /* 启动代理任务；-1=已在运行/资源不足 */
void mcp_stop(void);
int  mcp_running(void);

/* 传输侧投递一行 JSON（控制台调用）；0=已入队 1=通道满 2=超长拒绝 */
int mcp_submit_line(const char *line, uint32_t len);

#endif /* TUDOU_MCP_H */
