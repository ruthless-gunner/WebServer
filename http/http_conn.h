#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

// 一个 http_conn 对象就是一个客户连接
class http_conn
{
public:
    static const int FILENAME_LEN = 200;//设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;//设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;//设置写缓冲区m_write_buf大小
    //报文的请求方法，本项目只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST, //请求不完整，需要继续读取请求报文数据
        GET_REQUEST, //获得了完整的HTTP请求
        BAD_REQUEST, //HTTP请求报文有语法错误或请求资源为目录
        NO_RESOURCE, //请求资源不存在
        FORBIDDEN_REQUEST, //请求资源禁止访问，没有读取权限
        FILE_REQUEST, //请求资源可以正常访问
        INTERNAL_ERROR, //服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0, //读取完毕
        LINE_BAD, //此行有语法错误
        LINE_OPEN //表示接收不完整，buffer还需要继续接收
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);//关闭 http 连接，关闭一个连接，客户总量减一
    void process();
    bool read_once();//循环读取客户数据，直到无数据可读或对方关闭连接,非阻塞ET工作模式下，需要一次性将数据读完
    //响应报文写入函数
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    //这个版本的 init() 初始化对象的 private 变量
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    //解析http请求行
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //生成响应报文
    HTTP_CODE do_request();
    //get_line用于将指针向后偏移，指向第一个未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;//监听所有 http_conn 连接的epollfd
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];//存储读取的请求报文数据，即本线程的读缓冲区
    int m_read_idx;//缓冲区中m_read_buf中数据的最后一个字节的下一个位置，作为m_check_idx循环解析字符的终止位置
    int m_checked_idx;//指向 m_read_buf 中正在解析的字节，遇到m_read_idx时结束本次循环
    int m_start_line;//m_read_buf 中已经解析的字符个数
    char m_write_buf[WRITE_BUFFER_SIZE];//存储发出的响应报文数据，即本线程的写缓冲区
    int m_write_idx;//指示m_write_buf（响应报文的状态行、消息报头、空行）中已写的长度，此变量与要发送的文件（响应报文的响应正文）无关。只有在响应正文为错误信息时，才将整个响应报文写入m_write_buf

    CHECK_STATE m_check_state;//主状态机的状态
    METHOD m_method;//请求方法

    char m_real_file[FILENAME_LEN]; //服务器主机上存储待读取文件的绝对路径

    /*以下为解析请求报文中对应的6个变量*/
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length; //消息体字节数
    bool m_linger;//是否为长连接
    char *m_string; //存储请求报文的消息体

    char *m_file_address;//将服务器主机上的待读取文件（该文件的绝对地址为 m_real_file）映射到起始地址为 m_file_address 的内存中
    struct stat m_file_stat;//存储 m_real_file （服务器主机中存放的被请求访问的文件）的属性
    struct iovec m_iv[2];//io向量机制iovec
    int m_iv_count;
    int cgi;        //是否启用的POST
    int bytes_to_send;//剩余发送字节数
    int bytes_have_send;//已发送字节数
    char *doc_root; //网站根目录在服务器主机上的绝对路径，例如"/home/qgy/github/ini_tinywebserver/root"，其中"/home/qgy/github/ini_tinywebserver"就是程序当前的工作目录

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
