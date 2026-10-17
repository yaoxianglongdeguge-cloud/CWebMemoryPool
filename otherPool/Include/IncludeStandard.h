/*
 * 仅用于把 otherPool 的老池子“独立编出来做性能基准”时补齐最小系统头。
 * 线上是走 c_web/Include/IncludeStandard.h，这里不参与 cWeb 构建，只为 bench-other 目标服务。
 */
#ifndef OTHERPOOL_INCLUDE_STANDARD_H
#define OTHERPOOL_INCLUDE_STANDARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#endif
