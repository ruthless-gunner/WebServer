#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;//存储每对用户名和密码，key是用户名，value 是密码

//将数据库中的用户名和密码载入到服务器的map中来
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;//返回文件描述符的旧的状态标志，以便日后恢复该状态标志
    /*此函数要返回修改前的 fd 的属性，就必须用 new_option 变量先保存 fcntl() 的返回值
    int new_option = fcntl(fd, F_GETFL) | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return fcntl(fd, F_GETFL);//此时返回的是被修改过的 fd 的属性
    */
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;// EPOLLIN表示该套接字有数据可读，EPOLLRDHUP表示对方 TCP 请求关闭连接

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析buffer中的数据，将每行数据末尾的\r\n置为\0\0，并更新从状态机在buffer中读取的位置m_checked_idx，以此来驱动主状态机解析。
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    //m_read_idx 指向缓冲区m_read_buf的数据末尾的下一个字节
    //m_checked_idx 指向从状态机当前正在分析的字节
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];

        //如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }

        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        //当前字符既不是'\r'，又不是'\n'时，直接跳过当前字符，++m_check_idx
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)// 通信对方已关闭连接
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

/*
CHECK_STATE_REQUESTLINE:
    主状态机的初始状态，调用parse_request_line函数解析请求行;
    解析函数从m_read_buf中解析HTTP请求行，获得请求方法、目标URL及HTTP版本号;
    解析完成后主状态机的状态变为CHECK_STATE_HEADER。
*/
//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");//依次检验字符串 str1 中的字符(不包含空结束字符)，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符在str1中的下标
    //如果没有空格或\t，则报文格式有误
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    //将该位置改为\0，用于将前面数据取出
    *m_url++ = '\0';

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    if (strcasecmp(method, "GET") == 0)//比较是否相同，忽略大小写
        m_method = GET;////将请求报文的访问方法放在http_conn对象的m_method
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有（请求方法和 url 之间有若干个空格或'\t'的情况）
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");//返回 str1 中第一个不在字符串 str2 中出现的字符下标。
    
    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');//返回在str中第一次出现该字符的下标
    }
    //同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    //一般情况下，不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示欢迎界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");//将请求报文的url放在了http_conn对象的m_url

    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

/*
解析完请求行后，主状态机继续分析请求头。在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
CHECK_STATE_HEADER：
    调用parse_headers函数解析请求头部信息；
    判断是空行还是请求头，若是空行，进而判断content-length是否为0，如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，否则说明是GET请求，则报文解析结束;
    若解析的是请求头部字段，则主要分析connection字段，content-length字段，其他字段可以直接跳过，各位也可以根据需求继续分析;
    connection字段判断是keep-alive还是close，决定是长连接还是短连接；
    content-length字段，这里用于读取post请求的消息体长度;
*/
//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空行还是请求头
    if (text[0] == '\0')
    {
        //判断是GET还是POST请求
        if (m_content_length != 0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }

    //解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        //跳过空格和'\t'字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            //如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}
/*
CHECK_STATE_CONTENT:
    仅用于解析POST请求，调用parse_content函数解析消息体;
    用于保存post请求消息体，为后面的登录和注册做准备。
*/
//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //对于后续的登录和注册功能，为了避免将用户名和密码直接暴露在URL中，我们在项目中改用了POST请求，将用户名和密码添加在报文中作为消息体进行了封装。
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//process_read函数的返回值是对请求的文件分析后的结果，一部分是语法错误导致的BAD_REQUEST，一部分是do_request的返回结果
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*
    这里为什么这么写while的判断条件：
        1.在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。
        2.但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，这里转而使用主状态机的状态作为循环入口条件。
    那后面的&& line_status==LINE_OK又是为什么：
        1.解析完消息体后，报文的完整解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，也就是说，符合循环入口条件，还会再次进入循环，这并不是我们所希望的。
        2.为此，增加了该语句，并在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务。
    */
    //若当前读取的是请求报文的请求行、请求头、空行，则需要先调用 parse_line() 来解析将要读取的数据（将'\r'、'\n'换成'\0'，'\0'）,以便于主状态机直接取出对应字符串进行处理
    //若当前读取的是消息体，则直接读
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_checked_idx表示从状态机当前正在m_read_buf中解析的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);

        //主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            //解析请求行，只进入一次while循环
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            //解析请求头，每次while循环只读取一行请求头
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;

            //完整解析GET请求后，跳转到报文响应函数
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            //解析消息体，只进入一次while循环
            ret = parse_content(text);

            //完整解析POST请求后，跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();
            
            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status为LINE_OPEN
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
/*
为了更好的理解请求资源的访问流程，这里对本项目的各种页面跳转机制进行简要介绍。其中，浏览器网址栏中的字符，即url，可以将其抽象成ip:port/xxx，xxx通过html文件的action属性进行设置。
m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后的 m_url 有以下8种情况：
/   
    GET请求，跳转到judge.html，即欢迎访问页面
/0
    POST请求，跳转到register.html，即注册页面
/1
    POST请求，跳转到log.html，即登录页面
/2CGISQL.cgi
    POST请求，进行登录校验；
    验证成功跳转到welcome.html，即资源请求成功页面；
    验证失败跳转到logError.html，即登录失败页面。
/3CGISQL.cgi
    POST请求，进行注册校验；
    注册成功跳转到log.html，即登录页面；
    注册失败跳转到registerError.html，即注册失败页面。
/5
    POST请求，跳转到picture.html，即图片请求页面
/6
    POST请求，跳转到video.html，即视频请求页面
/7
    POST请求，跳转到fans.html，即关注页面
服务器根据 m_url 再设置真正的访问页面，此种方法不仅可以减少需要传输的字节；还可以为浏览器需要访问的资源进行加密，即使请求报文或响应报文（更底层的说法是：数据流或数据报）中途被人抓走了，也不知道用户想要访问什么资源
*/
//该函数将网站根目录和url文件拼接，然后通过stat判断该文件属性。另外，为了提高访问速度，通过mmap进行映射，将普通文件映射到内存逻辑地址。
http_conn::HTTP_CODE http_conn::do_request()
{
    //将 m_real_file 赋值为网站根目录
    strcpy(m_real_file, doc_root);//假设根目录为"/home/qgy/github/ini_tinywebserver/root"
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    //找到m_url中/的位置，进而判断/后第一个字符
    const char *p = strrchr(m_url, '/');//返回 str 中最后一次出现字符 c 的位置。如果未找到该值，则函数返回一个空指针。

    //处理cgi，实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))// m_url 为 /2CGISQL.cgi 或 /3CGISQL.cgi 时
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        // 例如：/2CGISQL.cgi 中的 2 只是区分注册还是登录的标志，真正用到的 url 是 /CGISQL.cgi
        //所以，下面5行代码利用 m_url_real 从 /2CGISQL.cgi 中把 /CGISQL.cgi 提取出来，然后将其拼接到 m_real_file 后面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);// 此时的 m_real_file 才是绝对路径，在此之前只是个目录
        free(m_url_real);// 有malloc，必须用 free 释放

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        //以&为分隔符，前面的为用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //以&为分隔符，后面的是密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //若没有重名的，则增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //判断map中能否找到重复的用户名
            if (users.find(name) == users.end())//没有重名
            {
                //向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);//查询成功，返回0.如果出现错误，返回非0值。
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)//注册成功跳转到log.html，即登录页面；
                    strcpy(m_url, "/log.html");
                else//注册失败跳转到registerError.html，即注册失败页面。
                    strcpy(m_url, "/registerError.html");
            }
            else//有重名
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);//!!
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);//!!
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/5，跳转到picture.html，即图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/6，跳转到video.html，即视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/7，跳转到fans.html，即关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果以上情况均不符合，则发送url实际请求的文件
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    /*
    int stat(const char *pathname, struct stat *statbuf)函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里，这里仅对其中用到的成员进行介绍
    struct stat 
    {
       mode_t    st_mode;        文件类型和权限 
       off_t     st_size;        文件大小，字节数
    };
    */
   //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体。失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);//将文件 fd 映射到内存，提高文件的访问速度。
    //避免文件描述符的浪费和占用
    close(fd);
    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/*
服务器子线程调用process_write完成响应报文（要发送的响应报文已经存在于http对象的成员变量m_iov[]中了），随后注册epollout事件。
服务器主线程检测写事件（注意：在本项目中，用的是 proactor 事件处理模式，所以该函数是由主线程调用），并调用http_conn::write函数将响应报文发送给浏览器端。

write 函数具体逻辑如下：

1、在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体
的指针和长度，并判断响应报文整体是否发送成功。
    若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接：
        若是长连接，则重置http类实例，注册读事件，不关闭连接;
        若是短连接，则直接关闭连接
2、若writev单次发送不成功，判断是否是写缓冲区满了。
    若不是因为缓冲区满了而失败，取消mmap映射，关闭连接；
    若eagain则是缓冲区满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到
    同一用户的下一请求，但可以保证连接的完整性。
*/
bool http_conn::write()
{
    int temp = 0;

    //若要发送的数据长度为0，则表示响应报文为空，但一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        /*
        writev函数将多块分散的内存数据一并写入文件描述符中，有时也将这该函数称为聚集写。

        #include <sys/uio.h>
        ssize_t writev(int filedes, const struct iovec *iov, int iovcnt);
            filedes表示文件描述符；
            iov为前述io向量机制结构体iovec；
            iovcnt为结构体的个数
        若成功则返回已写的字节数，若出错则返回-1。writev以顺序iov[0]，iov[1]至iov[iovcnt-1]从缓
        冲区中聚集输出数据。writev返回输出的字节总数，通常，它应等于所有缓冲区长度之和。
        
        特别注意： 循环调用writev时，需要重新处理iovec中的指针和长度，writev函数不会对这两个成员做任何处理。writev的返回值为已写的字节数。
        我们需要通过遍历iovec来计算新的基址，另外写入数据的“结束点”可能位于一个iovec的中间某个位置，因此需要调整临界iovec的io_base和io_len。
        */
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //发送失败（一个字节都没发出去），temp为发送的字节数
        if (temp < 0)
        {
            //判断缓冲区是否满了
            if (errno == EAGAIN)//缓冲区已满
            {
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        //更新已发送字节
        bytes_have_send += temp;
        //更新剩余发送字节
        bytes_to_send -= temp;

        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);//(bytes_have_send - m_write_idx)为指针在m_iov[1]中的偏移量：若此时刚好发完m_iov[0]，则 bytes_have_send 与 m_write_idx 相等，偏移量为0；若m_iov[1]以发送了部分数据，则 bytes_have_send - m_write_idx 表示的是从m_iov[1]中已经发送的字节数
            m_iv[1].iov_len = bytes_to_send;
        }
        //第一个iovec头部信息的数据还未发送完，继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            //取消mmap映射
            unmap();
            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //浏览器的请求为长连接
            if (m_linger)
            {
                //重新初始化HTTP对象
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

/*
根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文。
    add_status_line函数，添加状态行：http/1.1 状态码 状态消息;
    add_headers函数添加消息报头，内部调用add_content_length和add_linger函数:
        content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据;
        connection记录连接状态，用于告诉浏览器端保持长连接.
    add_blank_line添加空行.
上述涉及的5个函数，均是内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容。
*/
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx += len;
    //清空可变参列表
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//请求出错时，调用此函数直接将错误信息（响应正文）一并写入 m_write_buf 
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/*
响应报文分为两种:
若请求的文件存在，通过io向量机制iovec，声明两个iovec，第一个指向m_write_buf（m_write_buf 里是响应报文的状态行、消息报头、空行），
    第二个指向mmap的地址m_file_address（响应正文）。然后用 writev 函数将这两个地址的内容同时输出，这样可以避免把这两部分内容拼接到一起再发送。
若请求出错，这时候只申请一个iovec，指向m_write_buf（此时将响应正文（就是错误信息）写入m_write_buf 中，然后一并发送）。
*/
//响应报文写成功：此函数只是将响应报文的内容写入到了 m_iv[0]、m_iv[1] （若请求出错，只写入到了m_iv[0]）中；且将响应报文的总字节数存入到了成员变量 bytes_to_send 中
//对于含有请求资源的响应报文和错误信息的响应报文都是如此，若响应报文写失败：函数直接return false；并关闭当前套接字的连接；写成功的话，注册epollout事件，等待服务器主线程检测写事件并执行write函数
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        //内部错误，500
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500, error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        //报文语法有误，404
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求的资源大小为0，则返回空白html文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }

    //除FILE_REQUEST状态外，其余状态都只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        //注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    //响应报文写入失败，直接断开当前套接字的连接
    if (!write_ret)
    {
        close_conn();
    }

    //注册并监听写事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
