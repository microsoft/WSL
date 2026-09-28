/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtevent.c

Abstract:

    This file contains synchronization event primitive support. It enables
    simple synchronization across threads and forked processes.

--*/

#include "lxtevent.h"
#include "lxtlog.h"
#include <sys/mman.h>

int LxtSynchronizationEventClear(PLXT_SYNCHRONIZATION_EVENT Event)

/*++

Routine Description:

    This routine clears the synchronization event.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_lock(&Event->Lock));
    if (Event->Fail != 0)
    {
        LxtCheckResult(pthread_mutex_unlock(&Event->Lock));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Event->Ready = 0;
    LxtCheckResult(pthread_mutex_unlock(&Event->Lock));

ErrorExit:
    return Result;
}

int LxtSynchronizationEventDestroy(PLXT_SYNCHRONIZATION_EVENT* Event)

/*++

Routine Description:

    This routine frees all resources allocated for the event.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_destroy(&(*Event)->Lock));
    LxtCheckResult(pthread_mutexattr_destroy(&(*Event)->LockAttribute));
    LxtCheckResult(pthread_cond_destroy(&(*Event)->WaitConditionalVariable));
    LxtCheckResult(pthread_condattr_destroy(&(*Event)->ConditionVariableAttribute));
    LxtCheckResult(munmap(*Event, sizeof(*Event)));

ErrorExit:
    return Result;
}

int LxtSynchronizationEventFail(PLXT_SYNCHRONIZATION_EVENT Event)

/*++

Routine Description:

    This routine sets the fail flag and causes blocked event to be woken up and
    return with error and also any further calls to set/wait on the event to
    fail.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_lock(&Event->Lock));
    Event->Fail = 1;
    LxtCheckResult(pthread_mutex_unlock(&Event->Lock));
    LxtCheckResult(pthread_cond_signal(&Event->WaitConditionalVariable));

ErrorExit:
    return Result;
}

int LxtSynchronizationEventInit(PLXT_SYNCHRONIZATION_EVENT* Event)

/*++

Routine Description:

    This routine initializes synchronization event.

Arguments:

    Event - Supplies a pointer to where synchronization event will be allocated.

Return Value:

    0 on success, -1 on failure.

--*/

{
    void* MapResult;
    int Result = LXT_RESULT_FAILURE;
    PLXT_SYNCHRONIZATION_EVENT LocalEvent = NULL;

    LxtCheckMapErrno(LocalEvent = mmap(NULL, sizeof(*Event), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    LxtCheckResult(pthread_mutexattr_init(&LocalEvent->LockAttribute));
    LxtCheckResult(pthread_mutexattr_setpshared(&LocalEvent->LockAttribute, PTHREAD_PROCESS_SHARED));

    LxtCheckResult(pthread_mutex_init(&LocalEvent->Lock, &LocalEvent->LockAttribute));
    LxtCheckResult(pthread_condattr_init(&LocalEvent->ConditionVariableAttribute));
    LxtCheckResult(pthread_condattr_setpshared(&LocalEvent->ConditionVariableAttribute, PTHREAD_PROCESS_SHARED));

    LxtCheckResult(pthread_cond_init(&LocalEvent->WaitConditionalVariable, &LocalEvent->ConditionVariableAttribute));

    LocalEvent->Ready = 0;
    LocalEvent->Fail = 0;
    *Event = LocalEvent;
    Result = 0;

ErrorExit:
    return Result;
}

int LxtSynchronizationEventReset(PLXT_SYNCHRONIZATION_EVENT Event)

/*++

Routine Description:

    This routine resets event to the initialized state.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_lock(&Event->Lock));
    LxtCheckResult(pthread_cond_init(&Event->WaitConditionalVariable, &Event->ConditionVariableAttribute));

    Event->Fail == 0;
    Event->Ready = 0;
    LxtCheckResult(pthread_mutex_unlock(&Event->Lock));

ErrorExit:
    return Result;
}

int LxtSynchronizationEventSet(PLXT_SYNCHRONIZATION_EVENT Event)

/*++

Routine Description:

    This routine sets the synchronization event.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_lock(&Event->Lock));
    if (Event->Fail != 0)
    {
        LxtCheckResult(pthread_mutex_unlock(&Event->Lock));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Event->Ready = 1;
    LxtCheckResult(pthread_mutex_unlock(&Event->Lock));
    LxtCheckResult(pthread_cond_signal(&Event->WaitConditionalVariable));

ErrorExit:
    return Result;
}

int LxtSynchronizationEventWait(PLXT_SYNCHRONIZATION_EVENT Event)

/*++

Routine Description:

    This routine blocks on event until it is signalled.

Arguments:

    Event - Supplies a pointer to synchronization event.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result = LXT_RESULT_FAILURE;

    LxtCheckResult(pthread_mutex_lock(&Event->Lock));
    while ((Event->Ready == 0) && (Event->Fail == 0))
    {
        LxtCheckResult(pthread_cond_wait(&Event->WaitConditionalVariable, &Event->Lock));
    }

    if (Event->Fail != 0)
    {
        LxtCheckResult(pthread_mutex_unlock(&Event->Lock));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(pthread_mutex_unlock(&Event->Lock));

ErrorExit:
    return Result;
}
