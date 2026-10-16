"""E-02 判定实验回归用例：rfe 可控恢复（EPC1 + SR194）。

背景：docs/qemu-testing.md 旧结论"EPS1 不可访问 → rfe 无法可控恢复"
已被 2026-10-16 判定实验证伪。rfetest 命令在 ill 异常后由异常分发
把 EPC1 设为恢复点、候选 SR 设为异常前健康 PS，经 rfe 返回。
1611 变体额外证明 rfe 读 SR194 而非 SR161。恢复后 PS 正常的判据：
PASS 行 + 后续命令仍可响应（UART level-2 中断无恙）。
"""

READY_LINE = r"\[POTATO\] READY"
PASS_LINE = r"\[POTATO\]\[E2\] PASS"


def _run_variant(ctx, sr):
    ctx.serial.send(f"rfetest {sr}")
    ctx.serial.expect(r"\[POTATO\]\[E2\]\[H\] cause=0\(ILLEGAL_INSTRUCTION\)",
                      timeout=10)
    ctx.serial.expect(PASS_LINE, timeout=10)
    # PS 恢复健康：后续串口命令仍被 RX 中断正常接收处理
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=5)


def test_rfe_controlled_return(ctx):
    """rfe 从 EPC1+SR194 可控恢复：三种候选变体全部回到恢复点且 PS 健康。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    _run_variant(ctx, "161")   # 标准 EPS1 编号（rfe 不读它，但不影响恢复）
    _run_variant(ctx, "1611")  # 往 161 写 INTLEVEL=1：证明 rfe 读 194
    _run_variant(ctx, "194")   # rfe 实际读取的槽位
