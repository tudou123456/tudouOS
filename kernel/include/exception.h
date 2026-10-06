#ifndef TUDOU_EXCEPTION_H
#define TUDOU_EXCEPTION_H

/*
 * vectors.S 胶水调用，勿直接使用。
 * 参数：vec_flag 0=通用异常 1=双重异常；frame_base=本异常帧基址
 * （M3 起异常帧可整体转为被抢占任务的 IRQ 帧）。
 * 返回 1 = level-1 中断已处理，调用方恢复现场并 RFE；
 * 返回 0 = 致命异常，内部挂起（不返回到调用方语义）。
 * EXCCAUSE=1(syscall) 的 IRQ 帧恢复与 tick 抢占切换在内部完成（不返回）。
 */
unsigned exception_dispatch(unsigned vec_flag, void *frame_base);
void intc_unexpected_vector(unsigned vector_offset);

#endif /* TUDOU_EXCEPTION_H */
