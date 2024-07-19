//
// Created by jiangtao on 24-7-18.
//


#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif

#include "pthpool.h"

#ifdef THPOOL_DEBUG
#define THPOOL_DEBUG 1
#else
#define THPOOL_DEBUG 0
#endif

#if !defined(DISABLE_PRINT) || defined(THPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str)
#endif

#ifndef THPOOL_THREAD_NAME
#define THPOOL_THREAD_NAME thpool
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static volatile int threads_keepalive;
static volatile int threads_on_hold;

/*________________________结构体定义----------------------------*/

//二进制信号
typedef struct bsem{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int v;
}bsem;

//任务
typedef struct job{
    struct job*prev;    //前驱任务指针
    void (*function)(void*arg); //任务函数指针
    void *arg;  //函数参数
}job;

//任务队列
typedef struct jobqueue{
    pthread_mutex_t rwmutex;  //任务队列读写锁
    job* front;               //任务队列队头指针
    job* rear;                //任务队列队尾指针
    bsem* has_jobs;           //二进制信号标志
    int len;                  //任务队列长度,任务数量
}jobqueue;

//线程
typedef struct thread{
    int id;               //线程id
    pthread_t pthread;   //实际线程
    struct thpool_* thpool_p; //所属线程池
}thread;

//线程池
typedef struct thpool_{
    thread** threads;  //线程指针数组
    volatile int num_threads_alive; //线程存活的数量
    volatile int num_threads_working; //线程正在工作的数量
    pthread_mutex_t thcount_lock;   //正在使用线程的数量
    pthread_cond_t threads_all_idle;  //所以线程都属于空闲
    jobqueue jobqueue;  //任务队列
}thpool_;


/*---------------------------函数声明------------------------------*/
static int  thread_init(thpool_* thpool_p, struct thread** thread_p, int id);
static void* thread_do(struct thread* thread_p);
static void  thread_hold(int sig_id);
static void  thread_destroy(struct thread* thread_p);

static int jobqueue_init(jobqueue*jobqueue_p);
static void jobqueue_clear(jobqueue* jobqueue_p);
static void jobqueue_push(jobqueue* jobqueue_p,struct job*newjob);
static struct job* jobqueue_pull(jobqueue*jobqueue_p);
static struct job* jobqueue_pull(jobqueue*jobqueue_p);
static void jobqueue_destroy(jobqueue*jobqueue_p);

static void bsem_init(struct bsem*bsem_p,int value);
static void bsem_reset(bsem* bsem_p);
static void bsem_post(bsem*bsem_p);
static void bsem_post_all(bsem*bsem_p);
static void bsem_wait(bsem*bsem_p);


/*--------------------------线程池函数实现------------------------------*/

struct thpool_* thpool_init(int num_threads){
    threads_on_hold=0;
    threads_keepalive=1;

    if(num_threads<0){
        num_threads=0;
    }
    thpool_* thpool_p;
    thpool_p=(struct thpool_*)malloc(sizeof(struct thpool_));
    if(thpool_p==NULL){
        err("thpool_init:无法给线程池分配内存\n");
        return NULL;
    }
    thpool_p->num_threads_working=0;
    thpool_p->num_threads_alive=0;

    if(jobqueue_init(&thpool_p->jobqueue)==-1){
        err("thpool_init:无法初始化任务队列\n");
        free(thpool_p);
        return NULL;
    }

    thpool_p->threads=(struct thread**)malloc(num_threads*sizeof(struct thread*));
    if(thpool_p->threads==NULL){
        err("thpool_init:无法初始化线程\n");
        jobqueue_destroy(&thpool_p->jobqueue);
        free(thpool_p);
        return NULL;
    }
    pthread_mutex_init(&(thpool_p->thcount_lock), NULL);
    pthread_cond_init(&thpool_p->threads_all_idle, NULL);

   for(int i=0;i<num_threads;++i){
       thread_init(thpool_p,&thpool_p->threads[i],i);
#if THPOOL_DEBUG
       printf("THPOOL_DEBUG: Created thread %d in pool \n", n);
#endif
   }
   while(thpool_p->num_threads_alive!=num_threads){}
   return thpool_p;
}

//添加线程到线程池
int thpool_add_work(thpool_*thpool_p,void(*function_p)(void*),void*arg_p){
    job*newjob;
    newjob=(struct job*)malloc(sizeof(struct job));
    if(newjob==NULL){
        err("thpool_add_work;线程池添加线程时，分配内存失败\n");
        return -1;
    }

    newjob->function=function_p;
    newjob->arg=arg_p;
    jobqueue_push(&thpool_p->jobqueue,newjob);
    return 0;
}


//等待所有线程执行完
void thpool_wait(thpool_*thpool_p){
    pthread_mutex_lock(&thpool_p->thcount_lock);
    while(thpool_p->jobqueue.len||thpool_p->num_threads_working){
        pthread_cond_wait(&thpool_p->threads_all_idle,&thpool_p->thcount_lock);
    }
    pthread_mutex_unlock(&thpool_p->thcount_lock);
}

//销毁线程池
void thpool_destroy(thpool_*thpool_p){
    if(thpool_p==NULL) return;
    volatile int threads_total=thpool_p->num_threads_alive;
    threads_keepalive=0;
    double TIMEOUT=1.0;
    time_t start,end;
    double tpassed=0.0;
    time(&start);
    while(tpassed<TIMEOUT&&thpool_p->num_threads_alive){
        bsem_post_all(thpool_p->jobqueue.has_jobs);
        time(&end);
        tpassed= difftime(end,start);
    }

    while(thpool_p->num_threads_alive){
        bsem_post_all(thpool_p->jobqueue.has_jobs);
        sleep(1);
    }

    jobqueue_destroy(&thpool_p->jobqueue);
    for(int i=0;i<threads_total;++i){
        thread_destroy(thpool_p->threads[i]);
    }
    free(thpool_p->threads);
    free(thpool_p);
}

//线程池暂停
void thpool_pause(thpool_*thpool_p){
    for(int i=0;i<thpool_p->num_threads_alive;++i){
        pthread_kill(thpool_p->threads[i]->pthread,SIGUSR1);
    }
}

void thpool_resume(thpool_*thpool_p){
    (void)thpool_p;
    threads_on_hold=0;
}

int thpool_num_threads_working(thpool_* thpool_p){
    return thpool_p->num_threads_working;
}


/*--------------------------线程函数实现------------------------------*/

//初始化一个线程加入线程池
static int thread_init(thpool_*thpool_p,struct thread**thread_p,int id){
    *thread_p=(struct thread*)malloc(sizeof(struct thread));
    if(*thread_p==NULL){
        err("thread_init:无法为线程分配空间!\n");
        return -1;
    }
    (*thread_p)->thpool_p=thpool_p;
    (*thread_p)->id=id;
    pthread_create(&(*thread_p)->pthread,NULL,(void*(*)(void *))thread_do,(*thread_p));
    pthread_detach((*thread_p)->pthread);
    return 0;
}

//
static void thread_hold(int sig_id){
    (void)sig_id;
    threads_on_hold=1;
    while(threads_on_hold) {
        sleep(1);
    }
}

static void* thread_do(struct thread*thread_p){
    char thread_name[16] = {0};

    snprintf(thread_name, 16, TOSTRING(THPOOL_THREAD_NAME) "-%d", thread_p->id);

    /* Use prctl instead to prevent using _GNU_SOURCE flag and implicit declaration */
    prctl(PR_SET_NAME, thread_name);
    thpool_* thpool_p=thread_p->thpool_p;
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags=SA_ONSTACK;
    act.sa_handler=thread_hold;
    if(sigaction(SIGUSR1,&act,NULL)==-1){
        err("thread_do:不能处理SIGUSR1");
    }
    pthread_mutex_lock(&thpool_p->thcount_lock);
    thpool_p->num_threads_alive+=1;
    pthread_mutex_unlock(&thpool_p->thcount_lock);

  while(threads_keepalive) {
      bsem_wait(thpool_p->jobqueue.has_jobs);
      if (threads_keepalive) {
          pthread_mutex_lock(&thpool_p->thcount_lock);
          thpool_p->num_threads_working++;
          pthread_mutex_unlock(&thpool_p->thcount_lock);

          void (*func_buff)(void *);
          void *arg_buff;
          job *job_p = jobqueue_pull(&thpool_p->jobqueue);
          if (job_p) {
              func_buff = job_p->function;
              arg_buff = job_p->arg;
              func_buff(arg_buff);
              free(job_p);
          }
          pthread_mutex_lock(&thpool_p->thcount_lock);
          thpool_p->num_threads_working--;
          if (!thpool_p->num_threads_working) {
              pthread_cond_signal(&thpool_p->threads_all_idle);
          }
          pthread_mutex_unlock(&thpool_p->thcount_lock);

      }
  }
    pthread_mutex_lock(&thpool_p->thcount_lock);
    thpool_p->num_threads_alive--;
    pthread_mutex_unlock(&thpool_p->thcount_lock);
    return NULL;
}

//线程销毁
static void thread_destroy (thread* thread_p){
    free(thread_p);
}





/*--------------------------任务队列函数实现------------------------------*/
//初始化任务队列
static int jobqueue_init(jobqueue*jobqueue_p){
    jobqueue_p->len=0;
    jobqueue_p->front=NULL;
    jobqueue_p->rear=NULL;
    jobqueue_p->has_jobs=(struct bsem*)malloc(sizeof(struct bsem));
    if(jobqueue_p->has_jobs==NULL){
        return -1;
    }
    pthread_mutex_init(&(jobqueue_p->rwmutex),NULL);
    bsem_init(jobqueue_p->has_jobs,0);
    return 0;
}

//清空任务队列
static void jobqueue_clear(jobqueue* jobqueue_p){
    while(jobqueue_p->len){
        free(jobqueue_pull(jobqueue_p));
    }
    jobqueue_p->front=NULL;
    jobqueue_p->rear=NULL;
    bsem_reset(jobqueue_p->has_jobs);
    jobqueue_p->len=0;
}

//添加一个任务到任务队列中
static void jobqueue_push(jobqueue* jobqueue_p,struct job*newjob){
    pthread_mutex_lock(&jobqueue_p->rwmutex);
    newjob->prev=NULL;
    switch(jobqueue_p->len){
        case 0:
            jobqueue_p->front=newjob;
            jobqueue_p->rear=newjob;
            break;
        default:
            jobqueue_p->rear->prev=newjob;
            jobqueue_p->rear=newjob;
    }
    jobqueue_p->len++;
    bsem_post(jobqueue_p->has_jobs);
    pthread_mutex_unlock(&jobqueue_p->rwmutex);
}

//从任务队列中获取一个任务
static struct job* jobqueue_pull(jobqueue*jobqueue_p){
    pthread_mutex_lock(&jobqueue_p->rwmutex);
    job*job_p=jobqueue_p->front;
    switch(jobqueue_p->len){
        case 0:
            break;
        case 1:
            jobqueue_p->front=NULL;
            jobqueue_p->rear=NULL;
            jobqueue_p->len=0;
            break;
        default:
            jobqueue_p->front=job_p->prev;
            jobqueue_p->len--;
            bsem_post(jobqueue_p->has_jobs);
    }
    pthread_mutex_unlock(&jobqueue_p->rwmutex);
    return job_p;

}


//销毁任务队列
static void jobqueue_destroy(jobqueue*jobqueue_p){
    jobqueue_clear(jobqueue_p);
    free(jobqueue_p->has_jobs);
}


/*--------------------------二进制信号函数实现----------------------------*/
//信号初始化函数，传入值要求为0/1
static void bsem_init(struct bsem*bsem_p,int value){
    if(value<0||value>1){
        err("bsem_init():二进制信号的值只能是0或者1");
        exit(1);
    }
    pthread_mutex_init(&(bsem_p->mutex),NULL);
    pthread_cond_init(&(bsem_p->cond),NULL);
    bsem_p->v=value;
}

//信号重置为0
static void bsem_reset(bsem* bsem_p){
    pthread_mutex_destroy(&(bsem_p->mutex));
    pthread_cond_destroy(&(bsem_p->cond));
    bsem_init(bsem_p,0);
}

 //唤醒一个线程
static void bsem_post(bsem*bsem_p){
    pthread_mutex_lock(&bsem_p->mutex);
    bsem_p->v=1;
    pthread_cond_signal(&bsem_p->cond);
    pthread_mutex_unlock(&bsem_p->mutex);
}

//唤醒全部线程
static void bsem_post_all(bsem*bsem_p){
    pthread_mutex_lock(&bsem_p->mutex);
    bsem_p->v=1;
    pthread_cond_broadcast(&bsem_p->cond);
    pthread_mutex_unlock(&bsem_p->mutex);
}

//等到信号直到信号 v=0
static void bsem_wait(bsem*bsem_p){
    pthread_mutex_lock(&bsem_p->mutex);
    while(bsem_p->v!=1){
        pthread_cond_wait(&bsem_p->cond,&bsem_p->mutex);
    }
    bsem_p->v=0;
    pthread_mutex_unlock(&bsem_p->mutex);
}





