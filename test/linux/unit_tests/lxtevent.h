/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtevent.c

Abstract:

    This file contains synchronization event primitive support.

--*/

#include <pthread.h>

//
// Synchronization event usable for synchronizing threads and forked processes.
//

typedef struct _LXT_SYNCHRONIZATION_EVENT
{
    pthread_cond_t WaitConditionalVariable;
    pthread_condattr_t ConditionVariableAttribute;
    pthread_mutex_t Lock;
    pthread_mutexattr_t LockAttribute;
    int Ready;
    int Fail;
} LXT_SYNCHRONIZATION_EVENT, *PLXT_SYNCHRONIZATION_EVENT;

int LxtSynchronizationEventClear(PLXT_SYNCHRONIZATION_EVENT Event);

int LxtSynchronizationEventDestroy(PLXT_SYNCHRONIZATION_EVENT* Event);

int LxtSynchronizationEventFail(PLXT_SYNCHRONIZATION_EVENT Event);

int LxtSynchronizationEventInit(PLXT_SYNCHRONIZATION_EVENT* Event);

int LxtSynchronizationEventReset(PLXT_SYNCHRONIZATION_EVENT Event);

int LxtSynchronizationEventSet(PLXT_SYNCHRONIZATION_EVENT Event);

int LxtSynchronizationEventWait(PLXT_SYNCHRONIZATION_EVENT Event);

//
// Synchronization point based on synchronization event.
//

#define LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(_ChildPidVariable_) \
    PLXT_SYNCHRONIZATION_EVENT LxtSync##_ChildPidVariable_##Parent; \
    PLXT_SYNCHRONIZATION_EVENT LxtSync##_ChildPidVariable_##Child;

#define LXT_SYNCHRONIZATION_POINT_DECLARE_FOR_STATIC(_ChildPidVariable_) \
    static PLXT_SYNCHRONIZATION_EVENT LxtSync##_ChildPidVariable_##Parent; \
    static PLXT_SYNCHRONIZATION_EVENT LxtSync##_ChildPidVariable_##Child;

#define LXT_SYNCHRONIZATION_POINT_INIT_SYNCVARS(_ParentVar_, _ChildVar_) \
    (_ParentVar_) = NULL; \
    (_ChildVar_) = NULL; \
    LxtCheckResult(LxtSynchronizationEventInit(&_ParentVar_)); \
    LxtCheckResult(LxtSynchronizationEventInit(&_ChildVar_));

#define LXT_SYNCHRONIZATION_POINT_INIT_FOR(_ChildPidVariable_) \
    LXT_SYNCHRONIZATION_POINT_INIT_SYNCVARS(LxtSync##_ChildPidVariable_##Parent, LxtSync##_ChildPidVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_INIT() LXT_SYNCHRONIZATION_POINT_INIT_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_DESTROY_SYNCVARS(_ParentVar_, _ChildVar_) \
    LxtSynchronizationEventDestroy(&(_ParentVar_)); \
    LxtSynchronizationEventDestroy(&(_ChildVar_));

#define LXT_SYNCHRONIZATION_POINT_DESTROY_FOR(_ChildPidVariable_) \
    LXT_SYNCHRONIZATION_POINT_DESTROY_SYNCVARS(LxtSync##_ChildPidVariable_##Parent, LxtSync##_ChildPidVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_DESTROY() LXT_SYNCHRONIZATION_POINT_DESTROY_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_START_SYNCVARS(_ParentVar_, _ChildVar_) \
    LxtSynchronizationEventReset(_ChildVar_); \
    LxtSynchronizationEventReset(_ParentVar_);

#define LXT_SYNCHRONIZATION_POINT_START_FOR(_ChildPidVariable_) \
    LXT_SYNCHRONIZATION_POINT_START_SYNCVARS(LxtSync##_ChildPidVariable_##Child, LxtSync##_ChildPidVariable_##Parent)

#define LXT_SYNCHRONIZATION_POINT_START() LXT_SYNCHRONIZATION_POINT_START_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_END_SYNCVARS(_ChildId_, _ParentVar_, _ChildVar_, _Destroy_) \
    if (((_ChildId_) >= 0) && (Result < 0)) \
    { \
        LxtLogError("Failing synchronization points."); \
        LxtSynchronizationEventFail(_ChildVar_); \
        LxtSynchronizationEventFail(_ParentVar_); \
    } \
    if ((_ChildId_) > 0) \
    { \
        if (TEMP_FAILURE_RETRY(waitpid((_ChildId_), &Status, 0)) >= 0) \
        { \
            if (!WIFEXITED(Status)) \
            { \
                LxtLogInfo("Child exited uncleanly (Child = %d, Status = %x)", (_ChildId_), Status); \
                Result = LXT_RESULT_FAILURE; \
            } \
            else \
            { \
                Result = (int)(char)WEXITSTATUS(Status); \
            } \
        } \
        else \
        { \
            Result = errno; \
            LxtLogInfo("Failed wait on child %d with errno %d", (_ChildId_), Result); \
        } \
        if ((Result == LXT_RESULT_SUCCESS) && ((_ChildVar_)->Fail == 1)) \
        { \
            LxtLogInfo("Child failed"); \
            Result = LXT_RESULT_FAILURE; \
        } \
        if ((_Destroy_) != FALSE) \
        { \
            LXT_SYNCHRONIZATION_POINT_DESTROY_SYNCVARS(_ParentVar_, _ChildVar_) \
        } \
    } \
    else if ((_ChildId_) == 0) \
    { \
        _exit(Result); \
    }

#define LXT_SYNCHRONIZATION_POINT_END_FOR(_ChildPidVariable_, _Destroy_) \
    LXT_SYNCHRONIZATION_POINT_END_SYNCVARS((_ChildPidVariable_), LxtSync##_ChildPidVariable_##Parent, LxtSync##_ChildPidVariable_##Child, _Destroy_)

#define LXT_SYNCHRONIZATION_POINT_END() LXT_SYNCHRONIZATION_POINT_END_FOR(ChildPid, FALSE)

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD_SYNCVARS(_ParentVar_, _ChildVar_) \
    if (Result < 0) \
    { \
        LxtLogError("Failing synchronization points."); \
        LxtSynchronizationEventFail(_ChildVar_); \
        LxtSynchronizationEventFail(_ParentVar_); \
    }

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD_FOR(_SyncIdVariable_) \
    LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD_SYNCVARS(LxtSync##_SyncIdVariable_##Parent, LxtSync##_SyncIdVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD() LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT_SYNCVARS(_ThreadId_, _ParentVar_, _ChildVar_) \
    if (Result < 0) \
    { \
        LxtLogError("Failing synchronization points."); \
        LxtSynchronizationEventFail(_ChildVar_); \
        LxtSynchronizationEventFail(_ParentVar_); \
    } \
    if (((_ThreadId_) > 0) && (pthread_join((_ThreadId_), &Status) == 0)) \
    { \
        if (Status != 0) \
        { \
            LxtLogInfo("Thread exited uncleanly (Thread = %d, Status = %x)", (_ThreadId_), (int)(long)Status); \
            Result = LXT_RESULT_FAILURE; \
        } \
    } \
    else if ((_ThreadId_) > 0) \
    { \
        Result = errno; \
        LxtLogInfo("Failed wait on thread %d with errno %d", (_ThreadId_), Result); \
    } \
    if ((Result == LXT_RESULT_SUCCESS) && ((_ChildVar_)->Fail == 1)) \
    { \
        LxtLogInfo("Thread failed"); \
        Result = LXT_RESULT_FAILURE; \
    }

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT_FOR(_ThreadId_, _SyncIdVariable_) \
    LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT_SYNCVARS((_ThreadId_), LxtSync##_SyncIdVariable_##Parent, LxtSync##_SyncIdVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT(_ThreadId_) \
    LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT_FOR(_ThreadId_, ChildPid)

#define LXT_SYNCHRONIZATION_POINT_CLEAR_FOR(_ChildPidVariable_) \
    if (Result < 0) \
    { \
        LxtCheckResult(LxtSynchronizationEventClear(LxtSync##_ChildPidVariable_##Child)); \
        LxtCheckResult(LxtSynchronizationEventClear(LxtSync##_ChildPidVariable_##Parent)); \
    }

#define LXT_SYNCHRONIZATION_POINT_CLEAR() LXT_SYNCHRONIZATION_POINT_CLEAR_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_SYNCVARS(_IsChild_, _ParentVar_, _ChildVar_) \
    if ((_IsChild_) != FALSE) \
    { \
        LxtCheckResult(LxtSynchronizationEventWait(_ChildVar_)); \
        LxtCheckResult(LxtSynchronizationEventClear(_ChildVar_)); \
        LxtCheckResult(LxtSynchronizationEventSet(_ParentVar_)); \
    } \
    else \
    { \
        LxtCheckResult(LxtSynchronizationEventSet(_ChildVar_)); \
        LxtCheckResult(LxtSynchronizationEventWait(_ParentVar_)); \
        LxtCheckResult(LxtSynchronizationEventClear(_ParentVar_)); \
    }

#define LXT_SYNCHRONIZATION_POINT_CHILD_FOR(_ChildPidVariable_) \
    LXT_SYNCHRONIZATION_POINT_SYNCVARS(TRUE, LxtSync##_ChildPidVariable_##Parent, LxtSync##_ChildPidVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_PARENT_FOR(_ChildPidVariable_) \
    LXT_SYNCHRONIZATION_POINT_SYNCVARS(FALSE, LxtSync##_ChildPidVariable_##Parent, LxtSync##_ChildPidVariable_##Child)

#define LXT_SYNCHRONIZATION_POINT_FOR(_ChildPidVariable_) \
    if ((_ChildPidVariable_) == 0) \
    { \
        LXT_SYNCHRONIZATION_POINT_CHILD_FOR(_ChildPidVariable_); \
    } \
    else \
    { \
        LXT_SYNCHRONIZATION_POINT_PARENT_FOR(_ChildPidVariable_); \
    }

#define LXT_SYNCHRONIZATION_POINT_CHILD() LXT_SYNCHRONIZATION_POINT_CHILD_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT_PARENT() LXT_SYNCHRONIZATION_POINT_PARENT_FOR(ChildPid)

#define LXT_SYNCHRONIZATION_POINT() LXT_SYNCHRONIZATION_POINT_FOR(ChildPid);

//
// Declare global synchronization values for common ChildPid variable.
//

LXT_SYNCHRONIZATION_POINT_DECLARE_FOR_STATIC(ChildPid)