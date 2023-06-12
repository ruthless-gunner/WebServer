#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "debian-sys-maint";
    string passwd = "BO4yyzPKg5fpAUF2";
    string databasename = "yourdb";


    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志:通过单例模式获取唯一的日志类，调用init方法，初始化生成日志文件，服务器启动按当前时刻创建日志，
    //前缀为时间，后缀为自定义log文件名，并记录创建日志的时间day和行数count。
    //只是初始化，当后面的函数调用LOG_INFO等宏时，才开始写日志
    server.log_write();

    //数据库：单例模式实现
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}