/*
 * noop_mutex.h —— 仅用于基准：把老池子里的“加锁/解锁”变成空操作，
 * 用来回答“老池如果没有锁会多快”。
 *
 * 用法：把本文件用 -include 加进老池子的编译单元（见 test/Makefile）。
 * 注意：必须在 <pthread.h> 已经声明完之后再定义宏，否则会把头里的函数声明也改坏。
 * 这个变体只用于测量，绝不能用于生产（无锁共享 = 数据竞争）。
 */
#ifndef OTHERPOOL_NOOP_MUTEX_H
#define OTHERPOOL_NOOP_MUTEX_H

#include <pthread.h>

#define pthread_mutex_lock(m)   ((void)0)
#define pthread_mutex_unlock(m) ((void)0)

#endif
