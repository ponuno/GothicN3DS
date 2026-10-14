// sys.h for the PC build: POSIX threads.
#include "sys.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

namespace g3 {
namespace sys {

struct Thread {
	pthread_t handle;
	void (*entry)(void*);
	void* arg;
};

struct Mutex {
	pthread_mutex_t m;
};

struct Signal {
	pthread_mutex_t m;
	pthread_cond_t c;
	bool set;
};

static void* trampoline(void* p) {
	Thread* t = static_cast<Thread*>(p);
	t->entry(t->arg);
	return nullptr;
}

bool workersOnSecondCore() { return false; }

Thread* threadStart(void (*entry)(void*), void* arg) {
	Thread* t = static_cast<Thread*>(calloc(1, sizeof(Thread)));
	if (!t) return nullptr;
	t->entry = entry;
	t->arg = arg;
	if (pthread_create(&t->handle, nullptr, trampoline, t) != 0) {
		free(t);
		return nullptr;
	}
	return t;
}

void threadJoin(Thread* t) {
	if (!t) return;
	pthread_join(t->handle, nullptr);
	free(t);
}

Mutex* mutexCreate() {
	Mutex* m = static_cast<Mutex*>(calloc(1, sizeof(Mutex)));
	if (m) pthread_mutex_init(&m->m, nullptr);
	return m;
}

void mutexDestroy(Mutex* m) {
	if (!m) return;
	pthread_mutex_destroy(&m->m);
	free(m);
}

void mutexLock(Mutex* m) { pthread_mutex_lock(&m->m); }
void mutexUnlock(Mutex* m) { pthread_mutex_unlock(&m->m); }

Signal* signalCreate() {
	Signal* s = static_cast<Signal*>(calloc(1, sizeof(Signal)));
	if (!s) return nullptr;
	pthread_mutex_init(&s->m, nullptr);
	pthread_cond_init(&s->c, nullptr);
	return s;
}

void signalDestroy(Signal* s) {
	if (!s) return;
	pthread_cond_destroy(&s->c);
	pthread_mutex_destroy(&s->m);
	free(s);
}

void signalWait(Signal* s) {
	pthread_mutex_lock(&s->m);
	while (!s->set) pthread_cond_wait(&s->c, &s->m);
	s->set = false;
	pthread_mutex_unlock(&s->m);
}

void signalWake(Signal* s) {
	pthread_mutex_lock(&s->m);
	s->set = true;
	pthread_cond_signal(&s->c);
	pthread_mutex_unlock(&s->m);
}

double timeMs() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1.0e6;
}

} // namespace sys
} // namespace g3
