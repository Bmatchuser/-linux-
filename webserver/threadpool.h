#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <cstdio>
#include <exception>
using namespace std;

//线程池，定义为模板类是为了代码的复用
template <typename T>
class threadpool
{

public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    //添加任务的方法
    bool append(T* request);
private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void * worker(void * arg); 
    void run();

private:
    //线程的数量
    int m_thread_number;

    //线程池数组，大小为m_thread_number;
    pthread_t * m_threads;

    //请求队列中最多允许的等待处理的请求数量
    int m_max_requests;

    //请求队列
    list< T*> m_workqueue;

    //保护请求队列的互斥锁
    locker m_queuelocker;

    //信号量用来判断是否有任务需要处理；
    sem m_queuestat;

    //是否结束线程
    bool m_stop;


};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests), 
    m_stop(false), m_threads(NULL){

    if(m_thread_number <= 0 || m_max_requests <= 0){
        throw exception();
    }
    
    //创建线程
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw exception();
    }

    //创建thread_number个线程，并将他们设置为线程脱离
    for(int i = 0; i < thread_number; i++){

        printf("create the %dth request\n", i);

        if( pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete [] m_threads;
            throw exception();
        }
        //功能：分离所有创建的线程。被分离的线程在终止的时候，会自动释放资源返回给系统。
        if( pthread_detach(m_threads[i]) ){
            delete [] m_threads;
            throw exception();
        }
    }

}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true;
}

//append函数是向请求队列添加请求，所以没执行一次就需要信号量加1
template<typename T>
bool threadpool<T>::append(T* request){

    // 上锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        throw exception();
    }
    //向用户队列中添加用户
    m_workqueue.push_back(request);
    //信号量+1，run()函数中线程发现有用户来了，就开始进行处理。
    m_queuestat.post();
    m_queuelocker.unlock();
    
    return true;

}

template<typename T>
void * threadpool<T>::worker(void * arg){
    threadpool * pool = (threadpool *)arg;
    pool->run();
    return pool;
}

//run()函数就是处理操作，所以循环一次就需要信号量减1
template<typename T>
void threadpool<T>::run(){
    //当m_stop为false,说明线程在运行，这点很重要，只要线程未结束，就会不停的检测
    while(!m_stop){
        //如果-1后信号量为0，就会阻塞在这里，等待用户请求使信号量+1 才会被激活
        m_queuestat.wait();
        //信号量被激活，需要对请求进行处理，处理之前需要先上锁
        m_queuelocker.lock();
        //如果没有用户
        if(m_workqueue.empty()){ 
            m_queuelocker.unlock(); 
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request){
            continue;
        }

        request->process();
    }

}


#endif