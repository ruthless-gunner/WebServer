#ifndef LOG_H
#define LOG_H
//以单例模式创建Log类，日志类中的方法都不会被其他程序直接调用，末尾的四个可变参数宏提供了其他程序调用的方法。
#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    //C++11以后,使用局部静态变量懒汉不用加锁，因为C++11规定了local static在多线程条件下的初始化行为，要求编译器保证了内部静态变量的线程安全性。
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    //异步写日志公有方法，调用私有方法async_write_log
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    //init函数实现日志创建、写入方式的判断。可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    //init函数只由主线程调用，所以不用考虑多线程安全问题，而write_log函数需要考虑多线程安全问题
    //若为异步写日志，init函数还会创建一个阻塞队列和一个线程，这个线程会一直等待阻塞队列，然后从中取出一个日志string，写入文件，然后一直如此循环
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
 
    //write_log函数完成写入日志文件中的具体内容，主要实现日志分级、分文件、格式化输出内容。将输出内容按照标准格式整理
    //write_log函数需要考虑多线程安全问题
    //若为异步写日志，write_log则只是将Log对象的成员m_buf中的日志信息放入阻塞队列，然后init函数中创建的线程从阻塞队列中拿出日志消息，然后写入文件
    //若为同步写日志，write_log则直接将日志写入文件
    void write_log(int level, const char *format, ...);
 
    //强制刷新缓冲区
    void flush(void);

private:
    Log();
    virtual ~Log();

    //异步写日志方法
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);// c_str() 函数可以将 const string* 类型转化为 cons char* 类型,返回的是一个临时指针
            m_mutex.unlock();
        }
    }
    // // 删除编译器提供的默认的复制构造函数和赋值构造函数，避免单例被用户通过这两种方式构造对象
    // Log(const Log&) = delete;
	// Log& operator=(const Log&) = delete;

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;        //日志缓冲区首地址
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async;                  //是否异步标志位
    locker m_mutex;
    int m_close_log; //关闭日志
};

/*
日志类中的方法都不会被其他程序直接调用，末尾的四个可变参数宏提供了其他程序的调用方法。
前述四个方法对日志等级进行分类，包括DEBUG，INFO，WARN和ERROR四种级别的日志。
*/
//这四个宏定义在其他文件中使用，主要用于不同类型的日志输出
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
