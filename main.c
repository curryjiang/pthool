#include <stdio.h>


#include <pthread.h>
#include <stdint.h>
#include "pthpool.h"

void task(void *arg){
    printf("Thread #%u working on %ld\n", (int)pthread_self(), (long) arg);
}


int main(){

    puts("Making threadpool with 4 threads");
    threadpool thpool = thpool_init(4);

    puts("Adding 40 tasks to threadpool");
    int i;
    for (i=0; i<40; i++){
        thpool_add_work(thpool, task, (void*)(uintptr_t)i);
    };

    thpool_wait(thpool);
    puts("Killing threadpool");
    thpool_destroy(thpool);
    return 0;
}
