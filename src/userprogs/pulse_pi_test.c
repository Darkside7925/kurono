// pulsetest - isolate libpulse's threaded mainloop mutex on kurono (satoru).
#include <pthread.h>
#include <stdio.h>
#include <pulse/thread-mainloop.h>

static pthread_mutex_t rm;
static int g_bad = 0;
static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        int r = pthread_mutex_lock(&rm);
        if (r) { printf("PULSETEST worker lock %d\n", r); g_bad = 1; break; }
        pthread_mutex_unlock(&rm);
    }
    return 0;
}

int main(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(&rm, &a);
    printf("PULSETEST init=%d\n", r);
    for (int i = 0; i < 1000; i++) {
        r = pthread_mutex_lock(&rm);
        if (r) { printf("PULSETEST recursive lock %d\n", r); return 1; }
        r = pthread_mutex_lock(&rm);
        if (r) { printf("PULSETEST recursive lock2 %d\n", r); return 1; }
        pthread_mutex_unlock(&rm);
        pthread_mutex_unlock(&rm);
    }
    pthread_t t1, t2;
    pthread_create(&t1, 0, worker, 0);
    pthread_create(&t2, 0, worker, 0);
    pthread_join(t1, 0); pthread_join(t2, 0);
    printf("PULSETEST mutex %s\n", g_bad ? "FAIL" : "ok");

    pa_threaded_mainloop* m = pa_threaded_mainloop_new();
    printf("PULSETEST mainloop_new=%p\n", (void*)m);
    if (!m) return 1;
    printf("PULSETEST mainloop_start=%d\n", pa_threaded_mainloop_start(m));
    pa_threaded_mainloop_lock(m);
    printf("PULSETEST mainloop locked\n");
    pa_threaded_mainloop_unlock(m);
    printf("PULSETEST mainloop unlocked\n");
    pa_threaded_mainloop_stop(m);
    pa_threaded_mainloop_free(m);
    printf("PULSETEST PASS\n");
    return 0;
}
// end (satoru)
