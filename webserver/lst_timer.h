#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 64 // 读缓冲的大小
class util_timer; //前向声明

// 用户数据结构
struct client_data
{
    sockaddr_in address; //客户端socket地址
    int sockfd;          //socket文件描述符
    char buf[BUFFER_SIZE]; //读缓冲
    util_timer* timer;     //定时器
};

//定时器类
class util_timer
{
public:
    time_t expire;   //任务超时时间
    void (*cb_func)(client_data*); //任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data* user_date;
    util_timer* prev;  //指向前一个定时器
    util_timer* next;  //指向后一个定时器
public:
    util_timer(): prev(NULL), next(NULL){}
};

// 定时器链表，它是一个升序、双向链表，且带有头结点和尾结点
class sort_timer_lst
{
private:
    util_timer* head; //头结点
    util_timer* tail; //尾结点
private:
    /*
        一个重载的辅助函数，它被共有的add_timer 函数和 adjust_timer 函数调用
        该函数表示将目标定时器 timer 添加到节点lst_head 之后的部分链表中
    */
    void add_timer( util_timer* timer, util_timer* lst_head){
        util_timer* prev = lst_head;
        util_timer* tmp = lst_head->next;
        /*
            遍历list_head 节点之后的部分链表，直到找到一个超过时间大于目标定时器的超时时间节点
            并将目标定时器插入该节点之前    
        */
        while(tmp){
            if( timer->expire < tmp->expire){
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        /*
            如果遍历完lst_head节点之后的部分链表，仍未找到超时时间大于目标定时器的超时时间的节点，
            则将目标定时器插入链表尾部，并把它设置为链表的心得尾结点。
        */
       if(!tmp){
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
       }
    }
public:
    sort_timer_lst():head(NULL), tail(NULL){}
    
    // 链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst(){
        util_timer* tmp = head;
        while(tmp){
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    //将目标定时器timer添加到链表中
    void add_timer( util_timer* timer){
        if(!timer){
            return;
        }
        if(!head){
            head = tail = timer;
            return;
        }
        /* 如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部,作为链表新的头节点，
           否则就需要调用重载函数 add_timer(),把它插入链表中合适的位置，以保证链表的升序特性 */
        if( timer->expire < head->expire){
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }

    /*
        当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
        超时时间延长的情况，即该定时器需要往链表的尾部移动
    */
    void adjust_timer( util_timer* timer){
        if(!timer){
            return;
        }
        util_timer* tmp = timer->next;
        //如果被调整的目标定时器处在链表的尾部，或者该定时器新的超时时间值仍然小于其下一个定时器的超时时间则不用调整
        if( !tmp || timer->expire < tmp->expire){
            return;
        }
        //如果目标定时器是链表的头节点，则将该定时器从链表中取出并重新插入链表
        if(timer == head){
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }else{
            //如果目标定时器不是链表中的头节点，则将该定时器从链表中取出，然后插入其原来所在的位置后的部分链表中
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    //将目标定时器timer 从链表中删除
    void del_timer( util_timer* timer){
        if(!timer){
            return;
        }
        //如果只有一个定时器
        if( timer == head || timer == tail){
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        /* 如果链表中至少有两个定时器，且目标定时器是链表的头节点，
         则将链表的头节点重置为原头节点的下一个节点，然后删除目标定时器。 */
        if( timer == head){
            head = timer->next;
            head->prev = NULL;
            delete timer;
            return ;
        }
        /* 如果链表中至少有两个定时器，且目标定时器是链表的尾节点，
        则将链表的尾节点重置为原尾节点的前一个节点，然后删除目标定时器。*/
        if( timer == tail){
            tail = timer->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        // 如果目标定时器位于链表的中间，则把它前后的定时器串联起来，然后删除目标定时器
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // SIGALARM 信号每次被触发就在其信号处理函数中执行一次tick()函数，以处理链表上到期任务
    void tick(){
        if(!head){
            return;
        }
        printf("timer tick\n");
        time_t cur = time(NULL); //获取当前系统的时间
        util_timer* tmp = head;
        //从头结点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
        while(tmp){
            /* 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，
            比较以判断定时器是否到期*/
            if(cur < tmp->expire){
                return;
            }
            //调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_date);
            //执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
            head = tmp->next;
            if(head){
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

};
#endif