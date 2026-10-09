#ifndef TUDOU_MCP_H
#define TUDOU_MCP_H

/*
 * MCP 协议适配器（M6，支柱1 收官）——见 docs/mcp-bus-design.md。
 *
 * 传输绑定（当前：UART 行传输）：控制台识别 '{' 开头的行投递到
 * 消息基座通道；代理任务从通道取行 → JSON-RPC 解析 → 工具分发 →
 * 响应单行 JSON 回传。实时语义（有界/去重/回执）由消息基座提供。
 *
 * JSON-RPC 子集：单行请求 {"jsonrpc":"2.0","id":N,"method":"...",
 * "params":{...}}；响应 {"jsonrpc":"2.0","id":N,"result":{...}} 或
 * {"jsonrpc":"2.0","id":N,"error":{"code":C,"message":"..."}}。
 */

void mcp_init(void);          /* 通道初始化（内核启动期） */
int  mcp_start(void);         /* 启动代理任务；-1=已在运行/资源不足 */
void mcp_stop(void);
int  mcp_running(void);

/* 传输侧投递一行 JSON（控制台调用）；0=已入队 1=通道满 */
int mcp_submit_line(const char *line, uint32_t len);

#endif /* TUDOU_MCP_H */
