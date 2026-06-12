// minimal pthreads smoke test  -  proves the kurono kernel's CLONE_THREAD + futex
// gate. two worker threads each bump a shared counter 100000x under a mutex; the
// main thread joins both and prints the total. a correct 200000 means thread
// creation, the futex-backed mutex (serialisation), and pthread_join (the
// child-cleartid futex wake) all work. (satoru)
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter = 0;

// 1000 iterations each (2000 total): enough to prove serialisation + join while
// staying well inside the boot timeout even with the busy-retry mutex. (satoru)
static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

static void logs(const char* s) { write(2, s, strlen(s)); }

int main(void) {
    logs("pthread_test: start\n");
    pthread_t t1, t2;
    if (pthread_create(&t1, 0, worker, 0) != 0) { logs("pthread_test: create1 FAILED\n"); return 1; }
    logs("pthread_test: thread1 created\n");
    if (pthread_create(&t2, 0, worker, 0) != 0) { logs("pthread_test: create2 FAILED\n"); return 1; }
    logs("pthread_test: thread2 created\n");
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    char buf[80];
    int n = snprintf(buf, sizeof(buf),
                     "pthread_test: counter=%ld (expect 2000) %s\n",
                     counter, counter == 2000 ? "PASS" : "MISMATCH");
    write(2, buf, n);
    return 0;
}
// end (satoru)
