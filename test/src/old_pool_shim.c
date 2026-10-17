/*
 * old_pool_shim.c —— 给 otherPool 里的老池子套一层 void* 接口，
 * 这样它能和本仓库的新池子在“同一个进程、同一轮运行”里做对比。
 *
 * 同一个源文件编译两遍：
 *   -DSHIM_PREFIX=Old             → OldPool*        （老池原样，每级带 mutex）
 *   -DSHIM_PREFIX=OldNoLock       → OldNoLockPool*  （老池去锁变体，见 noop_mutex.h）
 * 两遍都配合同一套 -D 符号重命名（见 test/Makefile 的 bench-other 目标）；
 * 老池子的源码一个字节都没有改动（只在 otherPool/Include/ 下补了最小 include 替身）。
 */
#include "../../otherPool/MemoryPool/MemoryPool.h"

#ifndef SHIM_PREFIX
#define SHIM_PREFIX Old
#endif
#define SHIM_CAT2(a,b) a##b
#define SHIM_CAT(a,b) SHIM_CAT2(a,b)
#define SHIM(name) SHIM_CAT(SHIM_PREFIX,name)

int SHIM(PoolCreate)(void** pp,int max_num,int init_max,int strip_num_future,int init_strip_num)
{
    *pp = NULL;
    return MemoryPoolInit((Memory_Pool**)pp,max_num,init_max,strip_num_future,init_strip_num);
}

int SHIM(PoolAllocX)(void* p,int sizeKB,void** ptr,int* notfull)
{
    *ptr = NULL;
    return MemoryPoolAlloc((Memory_Pool*)p,sizeKB,ptr,notfull);
}

int SHIM(PoolFreeX)(void* p,void* ptr,int sizeBytes)
{
    if(ptr == NULL) return 1;
    return MemoryPoolFree((Memory_Pool*)p,ptr,sizeBytes);
}

/* 读取内部状态：基准里用来核对每级的条数/条大小/已用条数 */
void SHIM(PoolInfo)(void* p,int i,int* stripSizeKB,int* stripNum,int* hadedNum)
{
    Memory_Pool* mp = (Memory_Pool*)p;
    if(i < 0 || i >= mp->Entry_num){ *stripSizeKB = -1; *stripNum = -1; *hadedNum = -1; return; }
    *stripSizeKB = mp->pool[i].strip_size;
    *stripNum    = mp->pool[i].strip_num;
    *hadedNum    = mp->pool[i].haded_num;
}

int SHIM(PoolClasses)(void* p)
{
    return ((Memory_Pool*)p)->Entry_num;
}

/* 第 i 级的条内存基址（用于把指针换算成“第几条”，做重复发放检测） */
void* SHIM(PoolBase)(void* p,int i)
{
    Memory_Pool* mp = (Memory_Pool*)p;
    if(i < 0 || i >= mp->Entry_num) return NULL;
    return mp->pool[i].memory_strip;
}

/* 释放池自身的内存（老池子没有 destroy 接口），便于 valgrind 干净 */
void SHIM(PoolDestroy)(void* p)
{
    Memory_Pool* mp = (Memory_Pool*)p;
    if(mp == NULL) return;
    for(int i = 0;i < mp->Entry_num;i++){
        Memory_Entry* e = &mp->pool[i];
        if(e->memory_strip) free(e->memory_strip);
        if(e->stack){
            if(e->stack->leisure) free(e->stack->leisure);
            free(e->stack);
        }
    }
    free(mp->pool);
    free(mp);
}
