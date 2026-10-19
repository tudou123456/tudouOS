#ifndef TUDOU_DEMO_H
#define TUDOU_DEMO_H

/* M3 调度演示任务（kernel/kernel/demo.c），console 命令表使用 */
void demo_stop(void);
int demo_start(const char *mode);
const char *demo_mode_help(void);

#endif /* TUDOU_DEMO_H */
