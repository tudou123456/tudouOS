## 改动摘要

<!-- 一句话说明这个 PR 做了什么 -->

## 动机 / 关联 issue

<!-- 修复什么、为什么现在做；Closes #... -->

## 验证

- [ ] QEMU 干净构建 + 全量连续两轮：`py -3.14 tools/release_gate.py --rounds 2`
- [ ] 真机（如涉及）：`py -3.14 tools/release_gate.py --rounds 1 --hw-port COM3`
- [ ] 新增/修改的用例与日志锚点说明
- [ ] 文档已同步（命令、数字、兼容性）

结果粘贴：

```
QEMU: 全量通过（连续两轮）
HW: 0 FAIL / 0 TIMEOUT（如涉及真机）
```

## 风险与兼容性

<!-- 是否改 ABI/帧布局/命令输出/协议；旧固件/脚本是否需要调整 -->

## 提交前检查

- [ ] 没有提交 `build/`、`test/logs/`、`third_party/`、密钥或板卡 MAC
- [ ] `git diff --check` 通过；提交信息可读
- [ ] 涉及锁/中断/SMP 时说明了锁顺序与持锁约束
- [ ] 新增行为有可观测计数/追踪或测试覆盖
