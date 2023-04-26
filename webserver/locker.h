#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>
using namespace std;

//线程同步机制封装类

//互斥锁类
class locker{
private:
    pthread_mutex_t m_mutex;
public:

    locker(){
        if(pthread_mutex_init(&m_mutex, NULL) != 0){
            throw exception();
        }
    }

    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }

    //上锁
    bool lock(){
        return pthread_mutex_lock(&m_mutex);
    }

    //解锁
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex);
    }

    //获取互斥锁量
    pthread_mutex_t * get(){
        return &m_mutex;
    }
};

//条件变量类
class cond{
private:
    pthread_cond_t m_cond;
public:
    cond(){
        if(pthread_cond_init(&m_cond, NULL) != 0){
            throw exception();
        }
    }

    ~cond(){
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t * mutex){
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    bool timedwait(pthread_mutex_t * mutex, struct timespec t){
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    bool signal(pthread_mutex_t * mutex){
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast(pthread_mutex_t * mutex){
        return pthread_cond_broadcast(&m_cond) == 0;
    }

};

//信号量类
class sem{
private:
    sem_t m_sem;
public:
    sem(){
        if( sem_init(&m_sem, 0, 0) != 0){
            throw exception();
        }
    }

    sem(int num){
        if( sem_init(&m_sem, 0, num) != 0){
            throw exception();
        }
    }

    ~sem(){
        sem_destroy(&m_sem);
    }

    bool wait(){
        return sem_wait(&m_sem) == 0;
    }

    bool post(){
        return sem_post(&m_sem) == 0;
    }
};



#endif