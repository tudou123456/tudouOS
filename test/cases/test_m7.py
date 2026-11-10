"""M7 验收用例：以太网（open_eth MAC 后端 + ARP/IPv4/ICMP/UDP）与远程 MCP 通道。

里程碑验收（docs/architecture.md:126）：
  * ARP/ping 回环 —— QEMU `-nic user,model=open_eth` 的 OpenCores MAC 设备与
    SLIRP 网关（10.0.2.2）完成 ARP 解析与 ICMP 回显；
  * 断线重连后消息纪元（epoch）重同步 —— `net link down/up` 合成断线，
    内核进入新纪元；对端（test/net_peer.py）用旧纪元的请求被内核 resync
    回绝后自动跟随新纪元重发，闭环恢复。

用例依赖模块级 QEMU_ARGS：runner 会把它追加到该模块每个用例的 QEMU 命令行。
"""

import re
import time

from test.net_peer import Peer, mcp_request

READY_LINE = r"\[POTATO\] READY"
QEMU_ARGS = ["-nic", "user,model=open_eth,id=n0"]

# 行尾必须锚 \r：SerialTap.expect 在半行到达时就会命中，缺了它会在
# "link=u" 处匹配成功，取出 link="u" 而不是 "up"（M7 已知坑）。
NET_READY = r"\[POTATO\]\[NET\] ready mac=([0-9a-f:]{17}) ip=([0-9.]+) gw=([0-9.]+) udp=(\d+) link=(\w+)\r"


def _boot_net(ctx):
    """启动到「以太网就绪」，返回 NET ready 的正则匹配。

    注意顺序：main.c 里 net_start() 在启动横幅与 READY **之前**打印
    `[POTATO][NET] ready ...`，所以必须先等它再等 READY（SerialTap.expect
    从上次位置继续搜索，反了就会漏掉）。
    """
    m = ctx.serial.expect(NET_READY, timeout=20)
    ctx.serial.expect(READY_LINE, timeout=10)
    assert m.group(5) == "up", m.group(0)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    return m


def _net_stats(ctx):
    """发 `net` 并返回三段统计文本（解析成 dict 里的原文）。"""
    ctx.serial.send("net")
    ctx.serial.expect(r"\[POTATO\]\[NET\] present=\d", timeout=10)
    time.sleep(0.4)
    return ctx.serial.text


def test_m7_net_ready_and_link(ctx):
    """网卡就绪行 + 链路/设备状态自洽（含锁空闲断言）。"""
    m = _boot_net(ctx)
    mac, ip, gw = m.group(1), m.group(2), m.group(3)
    assert re.fullmatch(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", mac), mac
    assert ip == "10.0.2.15", ip          # SLIRP 默认租约
    assert gw == "10.0.2.2", gw
    text = _net_stats(ctx)
    assert "present=1" in text and "link=1" in text, text[-400:]
    assert "lock=free/0" in text, "net 锁未释放（M7 曾踩过发送阶段忘放锁）" + text[-400:]


def test_m7_net_arp_and_ping(ctx):
    """ARP 解析 + ICMP 回显（真实经过 QEMU 网卡与 SLIRP 网关）。"""
    _boot_net(ctx)
    ctx.serial.send("net arp")   # 注意：没有裸 `arp` 命令，子命令挂在 net 下
    ctx.serial.expect(r"\[POTATO\]\[NET\] arp (10\.0\.2\.2|[^\r\n]*empty)", timeout=10)
    ctx.serial.send("ping 10.0.2.2")
    m = ctx.serial.expect(
        r"\[POTATO\]\[NET\] ping 10\.0\.2\.2 (ok rtt=(\d+)ms|timeout)", timeout=20)
    assert "ok rtt=" in m.group(0), f"ping 未回环: {m.group(0)}"
    text = _net_stats(ctx)
    # 统计行字段顺序：arp(tx= rep_tx= rep_rx= miss=)
    assert re.search(r"arp\(tx=[1-9]\d* rep_tx=\d+ rep_rx=[1-9]\d*", text), text[-500:]
    assert re.search(r"icmp\(req=0 rep_rx=[1-9]\d* rep_tx=0\)", text), text[-500:]
    assert re.search(r"mac-tx=[1-9]\d*", text), text[-500:]
    # 缓存应记录网关
    assert re.search(r"arp 10\.0\.2\.2 [0-9a-f:]{17} age=\d+ms", text), text[-500:]


def test_m7_net_remote_mcp_roundtrip_and_dedup(ctx):
    """远程 MCP：hello 学习对端 → tools/list 经 UDP 往返 → 同 seq 重放。"""
    _boot_net(ctx)
    peer = Peer()
    try:
        ctx.serial.send(f"netmcp peer 10.0.2.2 {peer.port}")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] peer set", timeout=10)
        hello = peer.hello(timeout=10)
        assert hello.type == "hello" and "mac" in hello.body, hello.raw

        env, _seq = peer.call(mcp_request("tools/list", req_id=11), timeout=12,
                              ack=False)
        assert '"tools"' in env.body and '"name":"ping"' in env.body, env.body[:200]
        assert len(env.body) > 700, f"tools/list 响应过短: {len(env.body)}"
        assert env.epoch >= 1

        # 去重：未 ack 前同 seq 再发，内核只重放缓存，不重复执行
        peer.send_req(mcp_request("tools/list", req_id=11), seq=_seq)
        dup = peer.wait_for("resp", timeout=10, predicate=lambda e: e.seq == env.seq)
        assert dup.body == env.body, "重放响应与首次不一致"
        peer.send_ack(env.seq)

        # ack 之后槽被回收：同 seq 再问 → resync（不重复执行）
        peer.send_req(mcp_request("tools/list", req_id=11), seq=_seq)
        rs = peer.wait_for("resync", timeout=10)
        assert rs.epoch == env.epoch, (rs.epoch, env.epoch)

        ctx.serial.send("netmcp")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] rx=\d", timeout=10)
        time.sleep(0.3)
        dump = ctx.serial.text
        assert re.search(r"req=[1-9]\d* dup=[1-9]\d*", dump), dump[-400:]
        assert re.search(r"resync=[1-9]\d*", dump), dump[-400:]
    finally:
        peer.close()


def test_m7_net_reconnect_epoch_resync(ctx):
    """断线重连：epoch 递增 + 旧纪元请求被 resync 回绝 + 对端自动重同步。"""
    _boot_net(ctx)
    peer = Peer()
    try:
        ctx.serial.send(f"netmcp peer 10.0.2.2 {peer.port}")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] peer set", timeout=10)
        peer.hello(timeout=10)
        env1, _ = peer.call(mcp_request("ping", req_id=21), timeout=12)
        assert env1.epoch == 1, env1.epoch

        # 合成断线重连（QEMU 下 set_link 对 usernet 不稳定，用内核合成事件）
        ctx.serial.send("net link down")
        m = ctx.serial.expect(
            r"\[POTATO\]\[NETMCP\] link down epoch=(\d+) \(will bump\)", timeout=10)
        assert m.group(1) == "1", m.group(0)
        ctx.serial.send("net link up")
        m = ctx.serial.expect(r"\[POTATO\]\[NETMCP\] link up epoch=(\d+) pending=", timeout=10)
        assert m.group(1) == "2", f"重连后纪元未递增: {m.group(0)}"

        # 对端仍持旧纪元（1）：显式发一条旧纪元请求，内核丢弃并回 resync（不执行）
        peer.send_req(mcp_request("tools/list", req_id=22), seq=99, epoch=1)
        rs = peer.wait_for("resync", timeout=10)
        assert rs.epoch == 2, f"resync 应携带新纪元 2，实际 {rs.epoch}"

        # 对端跟随新纪元重发 → 正常执行（证明 resync 后可恢复，不是死路）
        peer.set_epoch(rs.epoch)
        env2, _ = peer.call(mcp_request("tools/list", req_id=22), timeout=12)
        assert env2.epoch == 2, f"重同步后纪元应为 2，实际 {env2.epoch}"
        assert '"tools"' in env2.body

        ctx.serial.send("netmcp")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] transport=", timeout=10)
        time.sleep(0.3)
        dump = ctx.serial.text
        assert re.search(r"epoch=2", dump), dump[-300:]
        # 内核把跨纪元残包计入 bad（resync 回执本身不单列发送计数，
        # 属内核观测口径：g_resyncs 只记「收到 resync/新纪元」）。
        assert re.search(r"bad=[1-9]\d*", dump), "跨纪元残包未计数" + dump[-300:]
        assert "stale epoch=1 (cur=2)" in dump, "内核未打印跨纪元丢弃锚点" + dump[-300:]
    finally:
        peer.close()


def test_m7_net_link_events_and_trace(ctx):
    """链路事件进追踪环（M8 观测）：TR_NET_LINK + 远程会话 TR_MCP_RESYNC。"""
    _boot_net(ctx)
    ctx.serial.send("net link down")
    ctx.serial.expect(r"\[POTATO\]\[NET\] link down", timeout=10)
    ctx.serial.send("net link up")
    ctx.serial.expect(r"\[POTATO\]\[NET\] link up", timeout=10)
    ctx.serial.send("trace imp 0")
    ctx.serial.expect(r"\[POTATO\]\[TRJ\] exported_imp=\d+", timeout=10)
    time.sleep(0.3)
    events = re.findall(r'\[POTATO\]\[TRJ\] (\{[^\r\n]*\})', ctx.serial.text)
    kinds = {}
    for e in events:
        m = re.search(r'"ev":"([A-Z_]+)"', e)
        if m:
            kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
    assert kinds.get("NET_LINK", 0) >= 2, f"链路事件未进副环: {kinds}"
    assert kinds.get("MCP_RESYNC", 0) >= 1, f"重连未记 MCP_RESYNC: {kinds}"


def test_m7_net_remote_mcp_hmac_auth(ctx):
    """UDP 信封 HMAC：配钥后无 mac/被篡改的请求被丢，合法请求仍可用。"""
    _boot_net(ctx)
    key = bytes(range(32))
    ctx.serial.send("netmcp key " + key.hex())
    ctx.serial.expect(r"auth key set", timeout=10)
    peer = Peer(key=key)
    try:
        ctx.serial.send(f"netmcp peer 10.0.2.2 {peer.port}")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] peer set", timeout=10)
        hello = peer.hello(timeout=10)
        assert hello.type == "hello", hello
        assert peer.auth_ok >= 1, "内核 hello 未带可验证 mac"

        env, _ = peer.call(mcp_request("ping", req_id=31), timeout=12)
        assert '"pong"' in env.body, env.body[:200]

        # 无 mac：内核在解析/状态变更前丢弃
        peer.send_req(mcp_request("ping", req_id=32), seq=90, sign=False)
        got = peer.drain(1.0)
        assert not any(e.type == "resp" and e.seq >= 90 for e in got), got

        # 有 mac 但 body 被篡改：验签失败，同样丢弃
        peer.send_req(mcp_request("ping", req_id=33), seq=91, tamper=True)
        got = peer.drain(1.0)
        assert not any(e.type == "resp" and e.seq >= 91 for e in got), got

        # 失败后合法请求仍工作（不是把会话打死）
        env2, _ = peer.call(mcp_request("ping", req_id=34), timeout=12)
        assert '"pong"' in env2.body, env2.body[:200]

        # SHA-256/HMAC 已知向量自检 + dump 计数
        ctx.serial.send("netmcp test")
        ctx.serial.expect(r"selftest ok", timeout=10)
        ctx.serial.send("netmcp")
        ctx.serial.expect(r"auth=hmac-sha256", timeout=10)
        time.sleep(0.3)
        dump = ctx.serial.text
        assert re.search(r"auth=hmac-sha256 ok=\d+ bad=[1-9]\d*", dump), dump[-400:]
    finally:
        peer.close()


def test_m7_net_remote_write_auth_and_quota(ctx):
    """UDP 写：无 key 拒绝授权；有 key 后按分钟配额放行/拒绝 fs.write。"""
    _boot_net(ctx)
    key = bytes([0xA5]) * 32

    # 无 key 时不允许把只读通道升级为可写
    ctx.serial.send("netmcp rw on")
    ctx.serial.expect(r"refuse remote write: set auth key first", timeout=10)

    ctx.serial.send("netmcp key " + key.hex())
    ctx.serial.expect(r"auth key set", timeout=10)
    peer = Peer(key=key)
    try:
        ctx.serial.send(f"netmcp peer 10.0.2.2 {peer.port}")
        ctx.serial.expect(r"\[POTATO\]\[NETMCP\] peer set", timeout=10)
        peer.hello(timeout=10)

        ctx.serial.send("netmcp quota 2")
        ctx.serial.expect(r"write quota=2/min", timeout=10)
        ctx.serial.send("netmcp rw on")
        ctx.serial.expect(r"remote write enabled", timeout=10)

        def write(name, data, rid):
            return peer.call(mcp_request(
                "tools/call",
                {"name": "fs.write",
                 "arguments": {"name": name, "data": data}},
                req_id=rid), timeout=12)[0]

        e0 = write("quota0.txt", "v0", 40)
        assert "written" in e0.body and "isError" not in e0.body, e0.body
        e1 = write("quota1.txt", "v1", 41)
        assert "written" in e1.body and "isError" not in e1.body, e1.body

        # 第 3 次超配额：拒绝且不落盘
        e2 = write("quota2.txt", "v2", 42)
        assert "write quota exceeded" in e2.body, e2.body
        assert '"isError":true' in e2.body, e2.body

        # 前两个文件可读，第 3 个不存在
        r0 = peer.call(mcp_request(
            "tools/call", {"name": "fs.read", "arguments": {"name": "quota0.txt"}},
            req_id=50), timeout=12)[0]
        assert "v0" in r0.body, r0.body
        r2 = peer.call(mcp_request(
            "tools/call", {"name": "fs.read", "arguments": {"name": "quota2.txt"}},
            req_id=51), timeout=12)[0]
        assert '"isError":true' in r2.body, r2.body

        ctx.serial.send("netmcp")
        ctx.serial.expect(r"quota=2 wdeny=1", timeout=10)
    finally:
        peer.close()
