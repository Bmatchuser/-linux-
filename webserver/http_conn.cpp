#include "http_conn.h" 

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


int http_conn::m_epollfd = -1;    //所有的socket上的事件都被注册到同一个epollfd；
int http_conn::m_user_count = 0;  //统计用户的数量

//网站的根目录
const char* doc_root = "/home/nowcoder/webserver1/resources";

//设置文件描述符非阻塞
int setnonblocking(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}


//添加文件描述符到epoll中
void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    // 内核2.6.17以后，对端如果异常断开会出现EPOLLIN和EPOLLRDHUP异常，这里直接通过事件去判断
    //event.events = EPOLLIN |  EPOLLRDHUP;
    event.events = EPOLLIN |  EPOLLRDHUP;
    if(one_shot){
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    setnonblocking(fd);
}

//从epoll中删除文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改epoll中的文件描述符,重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
extern void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//初始化新接收的连接
void http_conn::init(int sockfd, const sockaddr_in & addr){

    m_sockfd = sockfd;
    m_address = addr;

    //设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++; //用户数+1

    init(); 
}

void http_conn::init(){

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_check_index = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}

// 关闭连接
void http_conn::close_conn(){
    if(m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; //关闭一个连接，客户总数量-1
    }
}
//循环读取客户端数据，直到无数据刻度或者对方关闭连接
bool http_conn::read(client_data* &users2, int sockfd, sort_timer_lst &timer_lst, void(cb_func)(client_data*), int TIMESLOT ){
    
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }

    //读取到的字节
    int bytes_read = 0;
    while(true){


        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

        memset( users2[sockfd].buf, '\0', BUFFER_SIZE);
        printf("get %d bytes of client data %s from %d\n",bytes_read, users2[sockfd].buf,sockfd);
        util_timer* timer = users2[sockfd].timer;

        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                //没有数据
                break;
            }
            //如果发生了都错误，则关闭连接，并移除其对应的定时器
            if(errno != EAGAIN){
                cb_func( &users2[sockfd]);
                if(timer){
                    timer_lst.del_timer(timer);
                }
            }
            return false;
        }else if(bytes_read == 0){
            //对方关闭链接
            printf("client is closed!\n");
            //如果对方关闭了连接，我们也关闭连接，并移除对应的定时器
            cb_func(&users2[sockfd]);
            if(timer){
                timer_lst.del_timer(timer);
            }

            return false;
        }else{
            //如果某个客户端有数据可读，则我们要调正该连接对应的定时器，以延迟该连接被关闭的事件。
            if(timer){
                time_t cur = time(NULL);
                timer->expire = cur + 3*TIMESLOT;
                printf("adjust timer once\n");
                timer_lst.adjust_timer( timer);
            }
        }
        

        m_read_idx += bytes_read;
    }
    printf("读取到了数据：\n");
    //printf("%s\n", m_read_buf);
    return true;
}
// 写HTTP响应
bool http_conn::write(){
    printf("write+++++++++++++++++++++++++++++++++++++++\n");
    // printf("一次性写完数据\n");

    int temp = 0;

    if( bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1){
        //分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1){
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ){
                modfd( m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if(bytes_have_send > m_iv[0].iov_len){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len -= temp;
        }

        if(bytes_to_send <= 0){
            //没有数据要发了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if(m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }


    }
}
//释放内存映射
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}


//解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read(){

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
        // 解析到了一行完整的数据，或者解析到了请求体，也是完整的数据
        // 获取一行数据
        text = get_line();
        // 当前正在解析的行的起始位置就是当前正在分析的字符在读缓冲区中的位置
        m_start_line = m_check_index;
        //printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            //当前正在分析请求行
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            //当前正在分析请求头部字段
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            //当前正在解析请求体
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                //如果失败了，说明行数据尚且不完整
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    //如果前面的都没返回，则默认设置请求不完整，要继续读取客户信息。
    return NO_REQUEST;

}

// 解析HTTP请求行，获得请求方法，目标URL，HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char * text){
    
    //  GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");   // 判断第二个参数中的字符哪个在text中最先出现
    
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; // 置位空字符，\0为字符串结束符

    char * method = text;
    //  strcasecmp方法比较成功了是返回0
    if( strcasecmp(method, "GET") == 0){    
        m_method = GET;
    }else{
        return BAD_REQUEST;
    }
    //  /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    //  /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }
    //只比较7个字符，因为有的可能是http://192.168.110.129:10000/index.html
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');   // 找到m_url中第一次出现'/'的位置，返回的是指针。
    }

    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头

    return NO_REQUEST;
}   
// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char * text){
     //遇到空行，表示头部字段解析完毕
     if( text[0] == '\0'){
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        //状态机转移到CHECK_STATE_CONTENT
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则就说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
     }else if( strncasecmp(text, "Connection:", 11) == 0){
        //处理Connection头部字段， Connection:keep-alive
        text += 11;
        //strspn(text, " \t") 表示从text开始有连续多少个字符在" \t"能找到
        //比如说Connection:keep-alive
        //在"Connection:" 和 "keep-alive" 可能会有多个空格或者tab，需要跳过这些。
        text += strspn(text, " \t"); 
        if( strcasecmp(text, "keep-alive") == 0 ){
            m_linger = true;
        }
     }else if( strncasecmp (text, "Content-Length:", 15) == 0){
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
     }else if( strncasecmp (text, "Host:", 5) == 0){
        // 处理Host头部字段
        text += 5;
        text += strspn(text, "\t");
        m_host = text;
     }else{
        //printf("oop! unknow header %s\n",text);
     }
     
     return NO_REQUEST;
}
// 我们没有真正解析HTTP请求的消息体，只是判断请求体是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char * text){
    //如果 "从读缓冲区中读到的数据" 大于等于 "我请求体的数据" + "我当前已经检查的数据"
    //注意能走到这一步说明 "我当前已经检查的数据" 是包含请求行和请求头的
    //所以如果 >= 成立，说明从缓冲区中读到的数据包含了 请求行，请求头和请求体了
    if( m_read_idx >= ( m_content_length + m_check_index) ){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
//解析一行，判断依据是\r\n
http_conn::LINE_STATUS http_conn::parse_line(){

    char temp;
    // m_read_idx 在read()函数里读取用户信息以后才会增加，
    for(; m_check_index < m_read_idx; ++m_check_index){
        temp = m_read_buf[m_check_index];
        if( temp == '\r' ){
            if( m_check_index + 1 == m_read_idx){
                return LINE_OPEN;
            }else if(m_read_buf[m_check_index + 1] == '\n'){
                m_read_buf[m_check_index++] = '\0';
                m_read_buf[m_check_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(temp == '\n'){
            if((m_check_index > 1) && (m_read_buf[m_check_index - 1] == '\r')){
                m_read_buf[m_check_index - 1] = '\0';
                m_read_buf[m_check_index ++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //前面都未返回，说明数据不完整
    return LINE_OPEN;
}
// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    // "/home/nowcoder/webserver1/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //获取m_real_file文件的相关的状态信息，-1失败，0成功
    if(stat(m_real_file, &m_file_stat) < 0){
        return NO_RESOURCE;
    }

    //判断访问权限，如果没有权限就返回FORBIDDEN_REQUEST
    if( !(m_file_stat.st_mode & S_IROTH) ){
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录，如果是目录则返回BAD_REQUEST
    if( S_ISDIR( m_file_stat.st_mode ) ){
        return BAD_REQUEST;
    }

    //以只读方式打开文件，这个文件就是浏览器请求的文件
    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射，这个内存映射会在process_write()函数和write()函数中会用到，用以写出给浏览器
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;

}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            //处理响应请求行，并写会给用户端
            add_status_line( 500, error_500_title );
            //处理响应请求头，并写会给用户端
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //m_write_idx是请求行和请求头的大小，
            //m_file_stat.st_size是请求的文件内容的的大小,在这里就是index.html的内容大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 往写缓冲中写入返回的请求行和请求头
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    // 定义一具VA_LIST型的变量，这个变量是指向参数的指针；
    va_list arg_list;
    //初始化变量刚定义的VA_LIST变量
    va_start( arg_list, format );
    //将arg_list指针所指的参数，按照format格式赋值给m_write_buf数组 ，起点偏移m_write_idx，复制长度为WRITE_BUFFER_SIZE - 1 - m_write_idx
    //如果成功，返回值就是写入缓存区的字节长度
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    //用VA_END宏结束可变参数的获取。
    va_end( arg_list );
    return true;
}


// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){

    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        //重置该sockfd的EPOLLIN | EPOLLONESHOT 实践，继续监听
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    
    //printf("parse request, creat response\n");

    // 生成响应信息，这些信息会在调用write()函数时被写给浏览器
    bool write_ret = process_write( read_ret );
    //如果没有成功，那我就直接关闭连接，因为只有成功了才有后面的写回操作
    if( !write_ret ){
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
} 





