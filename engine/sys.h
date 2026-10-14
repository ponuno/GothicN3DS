// The little the engine needs from the operating system: one worker thread,
// a lock, a wake-up signal and a clock. Implemented per platform
// (platform/host/sys_host.cpp, platform/3ds/sys_3ds.cpp).
#pragma once
#include "g3ds/base.h"

namespace g3 {
namespace sys {

struct Thread;
struct Mutex;
struct Signal;

// Null if the platform cannot start a thread; the caller then does the work
// itself.
Thread* threadStart(void (*entry)(void*), void* arg);
// True if the worker threads run on the second application core (New 3DS only).
bool workersOnSecondCore();
void threadJoin(Thread* t); // waits for `entry` to return and frees `t`

Mutex* mutexCreate();
void mutexDestroy(Mutex* m);
void mutexLock(Mutex* m);
void mutexUnlock(Mutex* m);

// A flag one thread sets and another waits for; waiting clears it. A wake
// before the wait is not lost.
Signal* signalCreate();
void signalDestroy(Signal* s);
void signalWait(Signal* s);
void signalWake(Signal* s);

double timeMs(); // monotonic

} // namespace sys
} // namespace g3
