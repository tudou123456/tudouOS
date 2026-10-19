#ifndef TUDOU_MSGDEMO_H
#define TUDOU_MSGDEMO_H

/* M3.5 消息基座演示/压测（kernel/kernel/msgdemo.c），console 命令表使用 */
void msgdemo_stop(void);
int msgdemo_start(const char *mode, unsigned n);
void msgdemo_stat(void);
const char *msgdemo_mode_help(void);

#endif /* TUDOU_MSGDEMO_H */
