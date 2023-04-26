#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include <assert.h>
#include "lst_timer.h"

#define MAX_FD  65535 // 文件描述符的最大个数
#define MAX_EVENT_NUMBER 10000 //一次监听的最大的数量

#define FD_LIMIT 65535
#define TIMESLOT 5

static int pipefd[5];
static sort_timer_lst timer_lst;
static int epollfd = 0;

void sig_handler(int sig){
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void addsig(int sig ){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sigfillset( &sa.sa_mask);
    assert( sigaction(sig, &sa, NULL) != -1);
}

//添加信号捕捉
void addsig(int sig, void(handler)(int)){
    struct sigaction sa;
     //清空sa，都设置为0
    memset(&sa, '\0', sizeof(sa));
    //信号捕捉之后的处理函数
    sa.sa_handler= handler;
    //功能：将信号集中的所有的标志位置为1 ,设置信号集都是阻塞的
    sigfillset(&sa.sa_mask);
    // assert() 的用法很简单，我们只要传入一个表达式，它会计算这个表达式的结果：
    // 如果表达式的结果为“假”，assert() 会打印出断言失败的信息，并调用 abort() 
    // 函数终止程序的执行；如果表达式的结果为“真”，assert() 就什么也不做，程序继续往后执行。
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void timer_handler(){
    //定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    //因为一次alarm调用只会引起一次SIGALARM信号，所以我们要重新定时，以不断地出发SIGALARM信号。
    alarm(TIMESLOT);
}

//定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之
void cb_func(client_data* user_data){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert( user_data);
    close( user_data->sockfd);
    printf( "close fd %d\n", user_data->sockfd);
}



//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
//修改epoll中的文件描述符
extern void modfd(int epollfd, int fd, int ev);
extern int setnonblocking(int fd);
//arg表示参数的个数，argv[]是参数
int main(int argc, char * argv[]){

    if(argc <= 1){
        printf("按照如下格式运行： %s port_number\n", basename(argv[0])); //basename是获取基础的名字
    }

    //获取端口号 argv[0]是程序名， argv[1]是端口号
    int port = atoi(argv[1]);

    //网络中一段断开连接，而另一端还在写数据，可能导致SIGPIPE信号
    //对SIGPIPE信号进行处理,SIG_IGN是一个函数，表示忽略它
    //addsig(SIGPIPE, SIG_IGN);

    //创建线程池,http_conn是一个任务类
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }   
    catch(...)
    {
        return 1;
    }

    //创建一个数组，用于保存所有的客户端信息
    http_conn * users = new http_conn[ MAX_FD ];
    client_data* users2 = new client_data[FD_LIMIT];

    //下面就是TCP连接到基本步骤
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    bind(listenfd, (struct sockaddr *)&address, sizeof(address));


    //监听
    listen(listenfd, 5);

    //创建epoll对象，事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create(5);

    //将监听的文件描述符添加到epoll中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

     //创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1);
    setnonblocking( pipefd[1]);
    addfd( epollfd, pipefd[0], false);


    //设置信号处理函数,捕捉到相应的信号了就会运行相应处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );

    bool stop_server = false;
    bool timeout = false;

    printf("ssssssssssssssssssssss\n");
    alarm(TIMESLOT); //定时，5秒后产生SIGALARM信号

    while(!stop_server){
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if( (number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        //循环遍历事件数组
        for(int i = 0; i < number; i++){

            int sockfd = events[i].data.fd;

            if(sockfd == listenfd){
                printf("1\n");
                //有客户端链接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);

                if(connfd < 0){
                    printf( "errno is: %d\n", errno);
                    continue;
                }

                if(http_conn::m_user_count >= MAX_FD){
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙。
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组当中
                users[connfd].init(connfd, client_address);

                users2[connfd].address = client_address;
                users2[connfd].sockfd = connfd;
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_date = &users2[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                //设置超时时间为当前时间+15s
                timer->expire = cur + 3 * TIMESLOT; 
                users2[connfd].timer = timer;
                //添加定时器到链表
                timer_lst.add_timer(timer);

            }else if( (sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN) ){
                printf("2\n");
                //处理信号
                int sig;
                char signals[1024];
                int ret = recv( pipefd[0], signals, sizeof( signals ), 0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for(int i = 0; i < ret; i++){
                        switch(signals[i]){
                            case SIGALRM:{
                                //用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                //这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                printf("3\n");
                //对方异常断开或者错误等事件
                users[sockfd].close_conn();

            }else if(events[i].events & EPOLLIN){
                printf("4\n");
                if( users[sockfd].read(users2, sockfd, timer_lst, cb_func, TIMESLOT) ){         //一次性把所有的数据都读完
                    //交给线程去处理
                    pool->append(&users[sockfd]);
                }else {
                    users[sockfd].close_conn();
                }

            }else if(events[i].events & EPOLLOUT){
                printf("5\n");
                //如果写失败了
                if(!users[sockfd].write()){ //一次性写完所有的数据
                    users[sockfd].close_conn();
                }
            }
        }
        //最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if(timeout){
            timer_handler();
            timeout = false;
        }
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0]);
    delete []users2;

    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
    return 0;
}