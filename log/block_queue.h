/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;
阻塞队列是共享的资源，所以每个线程对阻塞队列进行写操作时，都要先申请互斥锁。只读操作可以不加互斥锁。
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
自定义的阻塞队列的好处是，线程会先从栈里加载函数，加载完毕后再申请互斥锁，再加锁；若直接使用STL的queue，由于queue的各种方法
的实现都没有加互斥锁，所以线程使用queue时，要先申请互斥锁，再加锁，然后才开始加载函数。
所以，相对于自定义的阻塞队列，直接使用STL的queue会延长线程持有互斥锁的时间，降低整个服务器的性能。
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

template <class T>
class block_queue
{
public:
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0)
        {
            exit(-1);
        }

        //构造函数创建循环数组
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~block_queue()
    {
        m_mutex.lock();
        if (m_array != NULL)
            delete [] m_array;

        m_mutex.unlock();
    }
    //判断队列是否满了
    bool full() 
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {

            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //判断队列是否为空
    bool empty() 
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //返回队首元素
    bool front(T &value) 
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }
    //返回队尾元素
    bool back(T &value) 
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_size;

        m_mutex.unlock();
        return tmp; //为什么不直接返回 m_size？
    }

    int max_size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_max_size;

        m_mutex.unlock();
        return tmp;//为什么不直接返回 m_max_size？
    }
    //往队列添加元素，需要将所有使用队列的线程先唤醒
    //当有元素push进队列,相当于生产者生产了一个元素
    //若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item)
    {

        m_mutex.lock();
        if (m_size >= m_max_size)
        {

            m_cond.broadcast();// 唤醒所有使用条件变量 m_cond 的线程
            m_mutex.unlock();
            return false;
        }

        //将新增数据放在循环数组的对应位置
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;

        m_size++;

        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }
    //pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item)
    {

        m_mutex.lock();

        //有多个消费者的时候，这里只能用while，不能用if;若只有一个消费者，则用 while 或 if 都可以
        while (m_size <= 0)
        {
            //当重新抢到互斥锁，pthread_cond_wait返回为0
            if (!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }

        //取出队列首的元素，这里需要理解一下，使用循环数组模拟的队列 
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    //增加了超时处理，在项目中没有使用到
    //在pthread_cond_wait基础上增加了等待的时间，只指定时间内能抢到互斥锁即可
    //其他逻辑不变
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();

        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t)) // 没有抢到互斥锁
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        //取出队列首的元素，这里需要理解一下，使用循环数组模拟的队列
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
