#ifndef TUDOU_POTATO_H
#define TUDOU_POTATO_H

#define POTATO_NAME    "tudouOS"
#define POTATO_KERNEL  "potato"
#define POTATO_VERSION "0.6.0"
#define POTATO_TARGET  "ESP32-S3"

/* 链接脚本导出的内存布局符号，kernel_main 里打印、测试台里断言 */
extern char _text_start[],  _text_end[];
extern char _data_start[],  _data_end[];
extern char _bss_start[],   _bss_end[];
extern char _stack_bottom[], _stack_top[];
extern char _heap_start[],  _heap_end[];

void kernel_main(void);

#endif /* TUDOU_POTATO_H */
