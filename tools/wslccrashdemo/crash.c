// Copyright (C) Microsoft Corporation. All rights reserved.
//
// The crashing workload for the wslc crash demo. It descends through several named frames before
// faulting, so the ELF core captured from the container carries a call stack worth unwinding
// instead of the single frame left behind by a process that was signalled from outside.
//
// The frames are named Test1 through Test6, outermost first, so a backtrace read out of the core
// is obviously this test program and the order of the descent can be checked at a glance.
//
// This is a Linux program: it is compiled inside the container, and the core it produces is an
// ELF core written by the guest kernel.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Each frame has to survive the compiler to appear in the core. noipa keeps the frame and blocks
// the interprocedural analysis that would otherwise clone these to take the loaded value and pull
// the dereference back up into the caller. Every caller also uses what its callee returned, so no
// call sits in tail position, which the optimizer would turn into a jump and erase from the stack.
#if defined(__has_attribute) && __has_attribute(noipa)
#define FRAME __attribute__((noipa))
#else
#define FRAME __attribute__((noinline))
#endif

FRAME static int Test6(const int* Sample)
{
    // The fault: Sample is null by the time it is dereferenced.
    return *Sample;
}

FRAME static int Test5(const int* Sample, int Scale)
{
    return Test6(Sample) * Scale;
}

FRAME static int Test4(const int* Sample, int Scale)
{
    const int decoded = Test5(Sample, Scale);

    return decoded + 1;
}

FRAME static int Test3(int Index)
{
    // The sample for record zero is missing, and nothing checks for it.
    const int* sample = (Index == 0) ? NULL : &Index;
    const int summary = Test4(sample, Index + 2);

    return summary + Index;
}

FRAME static int Test2(int Count)
{
    int total = 0;
    int index;

    // Counts down so the records with samples are processed first and the crash happens partway
    // through the batch rather than on the first call.
    for (index = Count - 1; index >= 0; index--)
    {
        printf("processing record %d\n", index);
        fflush(stdout);

        total += Test3(index);
    }

    return total;
}

FRAME static int Test1(int Count)
{
    return Test2(Count) + 1;
}

int main(int Argc, char** Argv)
{
    const int count = (Argc > 1) ? atoi(Argv[1]) : 3;
    int result;

    printf("pid %d: processing %d records\n", (int)getpid(), count);
    fflush(stdout);

    result = Test1(count);

    printf("workload returned %d\n", result);

    return 0;
}
