#ifndef TUDOU_MEMDEMO_H
#define TUDOU_MEMDEMO_H

/* M4 内存管理验收配套（kernel/kernel/memdemo.c），console 命令表使用 */
void memdemo_stop(void);
int memdemo_start(const char *mode, unsigned n);
const char *memdemo_mode_help(void);

#endif /* TUDOU_MEMDEMO_H */
