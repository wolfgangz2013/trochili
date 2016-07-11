/*************************************************************************************************
 *                                     Trochili RTOS Kernel                                      *
 *                                  Copyright(C) 2016 LIUXUMING                                  *
 *                                       www.trochili.com                                        *
 *************************************************************************************************/
#include <string.h>

#include "tcl.types.h"
#include "tcl.config.h"
#include "tcl.object.h"
#include "tcl.cpu.h"
#include "tcl.kernel.h"
#include "tcl.thread.h"
#include "tcl.debug.h"
#include "tcl.irq.h"

#if (TCLC_IRQ_ENABLE)

/* 中断向量描述符属性 */
#define IRQ_VECTOR_PROP_NONE   (TProperty)(0x0)
#define IRQ_VECTOR_PROP_READY  (TProperty)(0x1<<0)
#define IRQ_VECTOR_PROP_LOCKED (TProperty)(0x1<<1)

#if (TCLC_IRQ_DAEMON_ENABLE)
/* IRQ请求队列类型定义 */
typedef struct IrqListDef
{
    TObjNode* Handle;
} TIrqList;
#endif

/* 内核中断向量表 */
static TIrqVector IrqVectorTable[TCLC_IRQ_VECTOR_NUM];

/* MCU中断号到内核中断向量的转换表 */
static TAddr32 IrqMapTable[TCLC_CPU_IRQ_NUM];


/*************************************************************************************************
 *  功能：中断处理函数总入口                                                                     *
 *  参数：(1) irqn 中断号                                                                        *
 *  返回：无                                                                                     *
 *  说明：                                                                                       *
 *************************************************************************************************/
void xIrqEnterISR(TIndex irqn)
{
    TState      state;
    TError      error;
    TReg32      imask;

    TIrqVector* pVector;
    TISR        pISR;
    TArgument   data;
    TBitMask    retv = IRQ_ISR_DONE;

    KNL_ASSERT((irqn < TCLC_CPU_IRQ_NUM), "");
    CpuEnterCritical(&imask);

    /* 获得和中断号对应的中断向量 */
    pVector = (TIrqVector*)(IrqMapTable[irqn]);
    if ((pVector != (TIrqVector*)0) &&
            (pVector->Property & IRQ_VECTOR_PROP_READY))
    {
        /* 在处理中断对应的向量时，禁止其他代码修改向量 */
        pVector->Property |= IRQ_VECTOR_PROP_LOCKED;

        /* 在线程环境下调用低级中断处理函数 */
        if (pVector->ISR != (TISR)0)
        {
            pISR = pVector->ISR;
            data = pVector->Argument;
            CpuLeaveCritical(imask);
            retv = pISR(data);
            CpuEnterCritical(&imask);
        }

        /* 如果需要则调用中断处理线程ASR(用户中断处理线程或者内核中断守护线程),
           注意此时ASR可能处于eThreadReady状态 */
        if ((retv & IRQ_CALL_ASR) &&
                (pVector->ASR != (TThread*)0))
        {
            state = uThreadSetReady(pVector->ASR, eThreadSuspended, &error);
            state = state;
        }

        pVector->Property &= (~IRQ_VECTOR_PROP_LOCKED);
    }

    CpuLeaveCritical(imask);
}


/*************************************************************************************************
 *  功能：设置中断向量函数                                                                       *
 *  参数：(1) irqn     中断号                                                                    *
 *        (2) pISR     ISR处理函数                                                               *
 *        (3) pASR     中断处理线程                                                              *
 *        (4) data     应用提供的回调数据                                                        *
 *        (5) pError   详细调用结果                                                              *
 *  返回: (1) eFailure 操作失败                                                                  *
 *        (2) eSuccess 操作成功                                                                  *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState xIrqSetVector(TIndex irqn, TISR pISR, TThread* pASR, TArgument data, TError* pError)
{
    TState state = eFailure;
    TError error = IRQ_ERR_FAULT;
    TReg32 imask;
    TIndex index;
    TIrqVector* pVector;

    CpuEnterCritical(&imask);

    /* 如果指定的中断号已经注册过中断向量，那么直接更新 */
    if (IrqMapTable[irqn] != (TAddr32)0)
    {
        pVector = (TIrqVector*)(IrqMapTable[irqn]);

        /* 更新之前确保没有被锁定 */
        if ((pVector->Property & IRQ_VECTOR_PROP_LOCKED))
        {
            error = IRQ_ERR_LOCKED;
        }
        else
        {
            error = IRQ_ERR_NONE;
            state = eSuccess;
        }
    }
    else
    {
        /* 为该中断号申请中断向量项 */
        for (index = 0; index < TCLC_IRQ_VECTOR_NUM; index++)
        {
            pVector = (TIrqVector*)IrqVectorTable + index;
            if (!(pVector->Property & IRQ_VECTOR_PROP_READY))
            {
                /* 建立中断号和对应的中断向量的联系 */
                IrqMapTable[irqn] = (TAddr32)pVector;
                pVector->IRQn       = irqn;
                pVector->Property   = IRQ_VECTOR_PROP_READY;

                error = IRQ_ERR_NONE;
                state = eSuccess;
                break;
            }
        }
    }

    /* 设置中断向量对应的中断服务程序，中断服务线程(如果没有则默认为IrqDaemon线程) */
    if (state == eSuccess)
    {
#if (TCLC_IRQ_DAEMON_ENABLE)
        if (pASR == (TThread*)0)
        {
            pASR = uKernelVariable.IrqDaemon;
        }
#endif
        pVector->ISR      = pISR;
        pVector->ASR      = pASR;
        pVector->Argument = data;
    }

    CpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能：清空中断向量函数                                                                       *
 *  参数：(1) irqn 中断编号                                                                      *
 *  返回：无                                                                                     *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState xIrqCleanVector(TIndex irqn, TError* pError)
{
    TState state = eFailure;
    TError error = IRQ_ERR_FAULT;
    TReg32 imask;
    TIrqVector* pVector;

    CpuEnterCritical(&imask);

    /* 找到该中断向量并且清空相关信息 */
    if (IrqMapTable[irqn] != (TAddr32)0)
    {
        pVector = (TIrqVector*)(IrqMapTable[irqn]);
        if ((pVector->Property & IRQ_VECTOR_PROP_READY) &&
                (pVector->IRQn == irqn))
        {
            if (!(pVector->Property & IRQ_VECTOR_PROP_LOCKED))
            {
                IrqMapTable[irqn] = (TAddr32)0;
                memset(pVector, 0, sizeof(TIrqVector));
                error = IRQ_ERR_NONE;
                state = eSuccess;
            }
            else
            {
                error = IRQ_ERR_LOCKED;
            }
        }
    }

    CpuLeaveCritical(imask);

    *pError = error;
    return state;
}

#if (TCLC_IRQ_DAEMON_ENABLE)

/* IRQ守护线程定义和栈定义 */
static TThread IrqDaemonThread;
static TBase32 IrqDaemonStack[TCLC_IRQ_DAEMON_STACK_BYTES >> 2];

/* IRQ守护线程不接受任何线程管理API操作 */
#define IRQ_DAEMON_ACAPI (THREAD_ACAPI_NONE)

/* IRQ请求队列 */
static TIrqList IrqReqList;

/*************************************************************************************************
 *  功能：提交中断请求                                                                           *
 *  参数：(1) pIRQ      中断请求结构地址                                                         *
 *        (2) priority  中断请求优先级                                                           *
 *        (3) pEntry    中断处理回调函数                                                         *
 *        (4) data      中断处理回调参数                                                         *
 *  返回: (1) eFailure  操作失败                                                                 *
 *        (2) eSuccess  操作成功                                                                 *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState xIrqPostRequest(TIrq* pIRQ, TPriority priority, TIrqEntry pEntry, TArgument data,
                       TError* pError)
{
    TState state = eFailure;
    TError error = IRQ_ERR_FAULT;
    TReg32 imask;

    CpuEnterCritical(&imask);

    if (!(pIRQ->Property & IRQ_PROP_READY))
    {
        pIRQ->Property       = IRQ_PROP_READY;
        pIRQ->Entry          = pEntry;
        pIRQ->Argument       = data;
        pIRQ->Priority       = priority;
        pIRQ->ObjNode.Next   = (TObjNode*)0;
        pIRQ->ObjNode.Prev   = (TObjNode*)0;
        pIRQ->ObjNode.Handle = (TObjNode**)0;
        pIRQ->ObjNode.Data   = (TBase32*)(&(pIRQ->Priority));
        pIRQ->ObjNode.Owner  = (void*)pIRQ;
        uObjListAddPriorityNode(&(IrqReqList.Handle), &(pIRQ->ObjNode));

        error = IRQ_ERR_NONE;
        state = eSuccess;
    }

    CpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能：撤销中断请求                                                                           *
 *  参数：(1) pIRQ      中断请求结构地址                                                         *
 *  返回: (1) eFailure  操作失败                                                                 *
 *        (2) eSuccess  操作成功                                                                 *
 *  说明：                                                                                       *
 *************************************************************************************************/
TState xIrqCancelRequest(TIrq* pIRQ, TError* pError)
{
    TState state = eFailure;
    TError error = IRQ_ERR_UNREADY;
    TReg32 imask;

    CpuEnterCritical(&imask);
    if (pIRQ->Property & IRQ_PROP_READY)
    {
        uObjListRemoveNode( pIRQ->ObjNode.Handle, &(pIRQ->ObjNode));
        memset(pIRQ, 0, sizeof(TIrq));

        error = IRQ_ERR_NONE;
        state = eSuccess;
    }
    CpuLeaveCritical(imask);

    *pError = error;
    return state;
}


/*************************************************************************************************
 *  功能：内核中的IRQ守护线程函数                                                                *
 *  参数：(1) arg IRQ守护线程的用户参数                                                          *
 *  返回：无                                                                                     *
 *  说明：                                                                                       *
 *************************************************************************************************/
static void IrqDaemonEntry(TArgument argument)
{
    TState    state;
    TError    error;
    TIrq*     pIRQ;
    TIrqEntry pEntry;
    TArgument data;
    TReg32    imask;

    while(eTrue)
    {
        CpuEnterCritical(&imask);

        /* 如果IRQ请求队列为空则将IRQ守护线程挂起 */
        if (IrqReqList.Handle == (TObjNode*)0)
        {
            state = uThreadSetUnready(&IrqDaemonThread, eThreadSuspended, 0U, &error);
            state = state;
            CpuLeaveCritical(imask);
        }
        else
        {
            /* 从队列中逐个获得IRQ请求 */
            pIRQ   = (TIrq*)(IrqReqList.Handle->Owner);
            pEntry = pIRQ->Entry;
            data   = pIRQ->Argument;
            uObjListRemoveNode( pIRQ->ObjNode.Handle, &(pIRQ->ObjNode));
            memset(pIRQ, 0, sizeof(TIrq));
            CpuLeaveCritical(imask);

            /* 在线程环境下处理当前IRQ回调事务 */
            pEntry(data);
        }
    }
}


/*************************************************************************************************
 *  功能：初始化IRQ守护线程                                                                      *
 *  参数：无                                                                                     *
 *  返回：无                                                                                     *
 *  说明：                                                                                       *
 *************************************************************************************************/
void uIrqCreateDaemon(void)
{
    /* 检查内核是否处于初始状态 */
    if(uKernelVariable.State != eOriginState)
    {
        uDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }
	
    /* 初始化内核中断服务线程 */
    uThreadCreate(&IrqDaemonThread,
                  eThreadSuspended,
                  THREAD_PROP_PRIORITY_FIXED | \
                  THREAD_PROP_CLEAN_STACK | \
                  THREAD_PROP_DAEMON,
                  IRQ_DAEMON_ACAPI,
                  IrqDaemonEntry,
                  (TArgument)0,
                  (void*)IrqDaemonStack,
                  (TBase32)TCLC_IRQ_DAEMON_STACK_BYTES,
                  (TPriority)TCLC_IRQ_DAEMON_PRIORITY,
                  (TTimeTick)TCLC_IRQ_DAEMON_SLICE);

    /* 初始化相关的内核变量 */
    uKernelVariable.IrqDaemon = &IrqDaemonThread;
}

#endif


/*************************************************************************************************
 *  功能：定时器模块初始化                                                                       *
 *  参数：无                                                                                     *
 *  返回：无                                                                                     *
 *  说明：                                                                                       *
 *************************************************************************************************/
void uIrqModuleInit(void)
{
    /* 检查内核是否处于初始状态 */
    if(uKernelVariable.State != eOriginState)
    {
        uDebugPanic("", __FILE__, __FUNCTION__, __LINE__);
    }

    memset(IrqMapTable, 0, sizeof(IrqMapTable));
    memset(IrqVectorTable, 0, sizeof(IrqVectorTable));

#if (TCLC_IRQ_DAEMON_ENABLE)
    memset(&IrqReqList, 0, sizeof(IrqReqList));
#endif

    /* 初始化相关的内核变量 */
    uKernelVariable.IrqMapTable    = IrqMapTable;
    uKernelVariable.IrqVectorTable = IrqVectorTable;
}

#endif

