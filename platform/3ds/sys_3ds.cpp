// sys.h for the console: libctru threads and light synchronisation objects.
#include "sys.h"

#include <3ds.h>

#include <stdlib.h>

namespace g3 {
namespace sys {

struct Thread {
	::Thread handle;
	void (*entry)(void*);
	void* arg;
};

struct Mutex {
	LightLock lock;
};

struct Signal {
	LightEvent event;
};

static void trampoline(void* p) {
	Thread* t = static_cast<Thread*>(p);
	t->entry(t->arg);
}

static bool gWorkersOnCore2 = false;
bool workersOnSecondCore() { return gWorkersOnCore2; }

Thread* threadStart(void (*entry)(void*), void* arg) {
	Thread* t = static_cast<Thread*>(calloc(1, sizeof(Thread)));
	if (!t) return nullptr;
	t->entry = entry;
	t->arg = arg;
	// Same core as the main thread, one priority step below it: the worker
	// gets exactly the time the main thread spends waiting for the GPU and
	// the next frame, and never takes any away from it.
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	priority = priority < 0x3E ? priority + 1 : 0x3F;
	// New 3DS: the second application core (2) is free for the game; the workers (loader, read-ahead, sound, music)
	// run there for real instead of in the main thread's gaps. If it cannot be had (an old 3DS, or the system says
	// no) they stay on the main thread's core as before. (fix89; not tried on the console yet.)
	t->handle = nullptr;
	bool isNew = false;
	if (R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
		s32 spare = 0x30;
		svcGetThreadPriority(&spare, CUR_THREAD_HANDLE);
		t->handle = ::threadCreate(trampoline, t, 64 * 1024, spare < 0x3E ? spare + 1 : 0x3F, 2, false);
		if (t->handle) gWorkersOnCore2 = true;
	}
	if (!t->handle) t->handle = ::threadCreate(trampoline, t, 64 * 1024, priority, -2, false);
	if (!t->handle) {
		free(t);
		return nullptr;
	}
	return t;
}

void threadJoin(Thread* t) {
	if (!t) return;
	::threadJoin(t->handle, U64_MAX);
	::threadFree(t->handle);
	free(t);
}

Mutex* mutexCreate() {
	Mutex* m = static_cast<Mutex*>(calloc(1, sizeof(Mutex)));
	if (m) LightLock_Init(&m->lock);
	return m;
}

void mutexDestroy(Mutex* m) { free(m); }
void mutexLock(Mutex* m) { LightLock_Lock(&m->lock); }
void mutexUnlock(Mutex* m) { LightLock_Unlock(&m->lock); }

Signal* signalCreate() {
	Signal* s = static_cast<Signal*>(calloc(1, sizeof(Signal)));
	if (s) LightEvent_Init(&s->event, RESET_ONESHOT);
	return s;
}

void signalDestroy(Signal* s) { free(s); }
void signalWait(Signal* s) { LightEvent_Wait(&s->event); }
void signalWake(Signal* s) { LightEvent_Signal(&s->event); }

double timeMs() { return double(svcGetSystemTick()) / double(CPU_TICKS_PER_MSEC); }

} // namespace sys
} // namespace g3
