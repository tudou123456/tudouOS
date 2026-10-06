#ifndef TUDOU_EXCEPTION_H
#define TUDOU_EXCEPTION_H

/*
 * vectors.S 胶水调用，勿直接使用。
 * 返回 1 = level-1 中断已处理，调用方恢复现场并 RFE；
 * 返回 0 = 致命异常，内部挂起（不返回到调用方语义）。
 */
unsigned exception_dispatch(unsigned vec_flag);
void intc_unexpected_vector(unsigned vector_offset);

#endif /* TUDOU_EXCEPTION_H */
