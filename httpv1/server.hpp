#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cassert>
#include <sys/epoll.h>
#include <thread>
#include <mutex>
#include <sys/eventfd.h>
#include <memory>
#include <sys/timerfd.h>

#define INF 0
#define DBG 1
#define ERR 2
#define DEFAULT_LOG_LEVEL DBG

// level: 日志级别 (INF/DBG/ERR)
// format: 类似 printf 的格式化字符串，如 "hello %s"
// ... : 可变参数，配合 format 使用
#define LOG(level, format, ...)                                                                                           \
    {                                                                                                                     \
        /* 检查当前日志级别是否达到输出门槛 */                                                                            \
        if (level >= DEFAULT_LOG_LEVEL)                                                                                   \
        {                                                                                                                 \
            /* --- 获取并格式化当前时间 --- */                                                                            \
            time_t t = time(nullptr);                                                                                     \
            struct tm *m = localtime(&t);                                                                                 \
            char ts[32] = {0};                                                                                            \
            strftime(ts, 31, "%H:%M:%S", m);                                                                              \
            fprintf(stdout, "[%p %s %s:%d] " format "\n", (void *)pthread_self(), ts, __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                                                                 \
    }
#define INF_LOG(format, ...) LOG(LNF, format, ##__VA_ARGS__);
#define DBG_LOG(format, ...) LOG(DBG, format, ##__VA_ARGS__);
#define ERR_LOG(format, ...) LOG(ERR, format, ##__VA_ARGS__);

#define BUFFER_DEFAULT_SIZE 1024

class Buffer
{
private:
    std::vector<char> _buffer; // 使用vector进行内存空间管理
    uint64_t _reader_idx;      // 读偏移
    uint64_t _writer_idx;      // 写偏移
public:
    Buffer() : _reader_idx(0), _writer_idx(0), _buffer(BUFFER_DEFAULT_SIZE) {}
    // 获取底层空间的其实指针
    char *Begin() { return &*_buffer.begin(); }

    // 获取当前写入起始地址
    char *WritePosition() { return Begin() + _writer_idx; }

    // 获取当前读取起始地址
    char *ReadPosition() { return Begin() + _reader_idx; }

    // 获取缓冲区起始空闲空间大小 -- 写偏移之后的空闲空间
    uint64_t HandIdleSize() { return _reader_idx; }

    // 获取缓冲区末尾空闲空间大小 -- 读偏移之后的空闲空间
    uint64_t TailIdleSize() { return _buffer.size() - _writer_idx; }

    // 获取可读数据大小
    uint64_t ReadAbleSize() { return _writer_idx - _reader_idx; }

    // 将读偏移向后移动
    void MoveReadOffset(uint64_t len)
    {
        if (len == 0)
            return;
        assert(len <= ReadAbleSize());
        _reader_idx += len;
    }

    // 将写偏移向后移动
    void MoveWriteOffset(uint64_t len)
    {
        if (len == 0)
            return;
        assert(len <= TailIdleSize());
        _writer_idx += len;
    }

    // 确保可写空间足够（整体空闲空间够了就移动数据，否则就扩容）
    void EnsureWriteSpace(uint64_t len)
    {
        // 1. 如果末尾空闲空间大小足够，直接返回
        if (len <= TailIdleSize())
            return;
        // 2. 末尾空间不够，但加上头部的废弃空间足够，则进行数据“整理”
        if (len <= HandIdleSize() + TailIdleSize())
        {
            uint64_t res = ReadAbleSize();
            std::copy(ReadPosition(), ReadPosition() + res, Begin());
            _reader_idx = 0;
            _writer_idx = res;
        }
        else
        { // 3. 总体空间都不够，直接扩容底层 vector
            DBG_LOG("RESIZE &ld", _writer_idx + len);
            _buffer.resize(_writer_idx + len);
        }
    }
    /* ================= 写入相关接口 ================= */
    // 写入数据
    void Write(const void *data, uint64_t len)
    {
        if (len == 0)
            return;
        EnsureWriteSpace(len);
        const char *d = (const char *)data;
        std::copy(d, d + len, WritePosition());
    }

    // 写入数据并更新偏移量
    void WriteAndPush(const void *data, uint64_t len)
    {
        Write(data, len);
        MoveWriteOffset(len);
    }

    // 写入std::string 并更新偏移量
    void WriteAndPush(std::string &data, uint64_t len)
    {
        Write(data.c_str(), len);
        MoveWriteOffset(len);
    }

    // 从另一个Buffer 中读取数据并写入当前Buffer
    void WriteBuffer(Buffer &data)
    {
        Write(data.ReadPosition(), data.ReadAbleSize());
    }

    void WriteBufferAndPush(Buffer &data)
    {
        WriteBuffer(data);
        MoveWriteOffset(data.ReadAbleSize());
    }

    /* ================= 读取相关接口 ================= */

    // 读取数据到buf，但不一定读偏移量
    void Read(void *buf, uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::copy(ReadPosition(), ReadPosition() + len, (char *)buf);
    }

    // 读取数据到buf，并移动偏移量
    void ReadAndPop(void *buf, uint64_t len)
    {
        Read(buf, len);
        MoveReadOffset(len);
    }

    // 读取数据作为std::string 返回
    std::string ReadAsString(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0], len);
        return str;
    }

    // 读取数据作为std::string 返回，并使用读偏移量
    std::string ReadAsStringAndPop(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str = ReadAsString(len);
        MoveReadOffset(len);
        return str;
    }

    // 查找换行符 '\n'，常用于解决 TCP 粘包/半包问题中的按行读取
    char *FindCRLF()
    {
        char *res = (char *)memchr(ReadPosition(), '\n', ReadAbleSize());
        return res;
    }

    // 获取一行数据 (包括换行符)，但不移动偏移量
    std::string GetLine()
    {
        char *pos = FindCRLF();
        if (pos == nullptr)
        {
            return "";
        }
        return ReadAsString(pos - ReadPosition() + 1);
    }

    // 获取一行数据并移除
    std::string GetLineAndPop()
    {
        std::string str = GetLine();
        MoveReadOffset(str.size());
        return str;
    }

    // 清空缓冲区
    void Clear()
    {
        _reader_idx = 0;
        _writer_idx = 0;
    }
};

#define MAX_LISTEN 2024
// Socket封装类
// 对TCP套接字常用操作进行简单封装：
// 创建、绑定、监听、连接、收发数据、关闭、设置非阻塞等

class Socket
{
private:
    int _sockfd; // 套接字文件描述符
public:
    Socket() : _sockfd(-1) {}
    Socket(int fd) : _sockfd(fd) {}
    ~Socket() { Close(); }

    // 获取套接字描述符
    int Fd() { return _sockfd; }

    // 创建套接字
    bool Create()
    {
        // int socket(int domain,int type,int protocol)
        _sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (_sockfd < 0)
        {
            ERR_LOG("CREATE SOCKET FAILED!!");
            return false;
        }
        return true;
    }

    // 绑定地址信息
    bool Bind(const std::string &ip, uint16_t port)
    {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        int st = inet_pton(AF_INET, ip.c_str(), &addr);
        socklen_t len = sizeof(struct sockaddr_in);
        // int bind(int sockfd,const struct sockaddr *addr,socklen_t addrlen);
        int ret = bind(_sockfd, (struct sockaddr *)&addr, len);
        if (ret < 0)
        {
            ERR_LOG("BIND ADDRESS FAILED!");
            return false;
        }
        return true;
    }

    // 开始监听
    bool Listen(int backlog = MAX_LISTEN)
    {
        // int listen(int sockfd,int backlog)
        int ret = listen(_sockfd, backlog);
        if (ret < 0)
        {
            ERR_LOG("SOCKET LISTEN FAILED!");
            return false;
        }
        return true;
    }

    // 向服务器发起连接
    bool Connect(const std::string &ip, uint16_t port)
    {
        // int connect(int sockfd,const struct sockaddr *addr,socklen_t addrlen);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        int st = inet_pton(AF_INET, ip.c_str(), &addr.sin_addr.s_addr);
        socklen_t len = sizeof(struct sockaddr_in);
        int ret = connect(_sockfd, (struct sockaddr *)&addr, len);
        if (ret < 0)
        {
            ERR_LOG("CONNECT SERVER FAILED");
            return false;
        }
        return true;
    }

    // 获取新连接
    bool Accept()
    {
        // int accept(int sockfd,struct sockaddr *_Nullable restrict addr,socklen_t *_Nullable restrict addrlen)
        // 返回新的通信套接字
        int newfd = accept(_sockfd, nullptr, nullptr);
        if (newfd < 0)
        {
            ERR_LOG("SOCKET ACCEPT FAILED!");
            return -1;
        }
        return newfd;
    }

    // 接收数据
    ssize_t Recv(void *buf, size_t len, int flag = 0)
    {
        // ssize_t recv(int sockfd,void *buf,size_t len,int flag);
        ssize_t ret = recv(_sockfd, buf, len, flag);
        if (ret <= 0)
        {
            // EAGAIN 当前socket的接收缓冲区中没有数据，在非阻塞情况下会有这个错误
            // EINTR 表示当前socket阻塞等待，被信号打断了
            if (errno == EAGAIN || errno == EINTR)
            {
                return 0; // 表示这次没有收到数据
            }
            // ret == 0:对端关闭连接
            // ret<0: 发生错误
            ERR_LOG("SOCKET RECV FAILED!!");
            return -1;
        }
        return ret; // 实际接收的数据长度
    }

    // 非阻塞接收
    ssize_t Send(void *buf, size_t len, int flag = 0)
    {
        // ssize_t send(int sockfd, const void buf[.len], size_t len, int flags);
        int ret = send(_sockfd, buf, len, flag);
        if (ret < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                return 0;
            }
            ERR_LOG("SOCKET SEND FAILED!!");
            return -1;
        }
        return ret; // 实际发送长度
    }

    // 非阻塞发送
    ssize_t NonBlockSend(void *buf, ssize_t len)
    {
        if (len == 0)
            return 0;
        return Send(buf, len, MSG_DONTWAIT);
    }

    // 关闭套接字
    void Close()
    {
        if (_sockfd != -1)
        {
            close(_sockfd);
            _sockfd = -1;
        }
    }

    // 创建一个服务端连接
    bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0", bool block_flag = false)
    {
        // 1.创建套接字 2.绑定地址 3.开始监听 4.设置非阻塞 5.启动地址重用
        if (!Create())
            return false;
        if (!block_flag)
            NonBlock();
        if (!Bind(ip, port))
            return false;
        if (!Listen())
            return false;
        ReuseAddress();
        return true;
    }

    // 创建一个客户端连接
    bool CreateClient(uint16_t port, const std::string &ip)
    {
        // 1.创建套接字 2.指向连接服务器
        if (!Create())
            return false;
        if (!Connect(ip, port))
            return false;
        return true;
    }

    // 设置套接字选项--开启地址端口重用
    void ReuseAddress()
    {
        // int setsockopt(int sockfd, int level, int optname,const void optval[.optlen],socklen_t optlen);
        int val = 1;
        // 允许TIME_WAIT状态下快速重用地址
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&val, sizeof(int));
        int val = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, (void *)&val, sizeof(int));
    }

    // 设置套接字阻塞属性为非阻塞
    void NonBlock()
    {
        // int fcntl(int fd, int op, ... /* arg */ );
        int flag = fcntl(_sockfd, F_GETFL, 0);
        // 增加O_NONBLOCK标志
        fcntl(_sockfd, F_SETFL, flag | O_NONBLOCK);
    }
};

class Poller;
class EventLoop;
// Channel类：对文件描述符（fd）及其相关事件、回调函数的封装模块（通常用于Reactor模式）

class Channel
{
private:
    int _fd;           // 该Channel负责封装管理的文件描述符
    EventLoop *_loop;  // 该Channel所属的EventLoop（事件循环）实例
    uint32_t _events;  // 当前需要监控的事件
    uint32_t _revents; // 当前连接触发的事件

    // 事件回调函数类型声明
    using EventCallBack = std::function<void()>;
    EventCallBack _read_callback;  // 可读事件被触发的回调函数
    EventCallBack _write_callback; // 可写事件被触发的回调函数
    EventCallBack _error_callback; // 错误事件被触发的回调函数
    EventCallBack _close_callback; // 连接断开事件被触发的回调函数
    EventCallBack _event_callback; // 任意事件被触发的回调函数
public:
    Channel(EventLoop *loop, int fd) : _fd(fd), _events(0), _revents(0), _loop(loop) {}

    // 获取当前Channel管理的文件描述符
    int Fd() { return _fd; }

    // 获取想要监控的事件
    uint32_t Events() { return _events; }

    // 设置实际就绪的事件
    uint32_t SetREvents(uint32_t events) { _revents = events; }

    // 注册具体的事件回调函数
    void SetReadCallBack(const EventCallBack &cb) { _read_callback = cb; }
    void SetWriteCallBack(const EventCallBack &cb) { _write_callback = cb; }
    void SetErrorCallBack(const EventCallBack &cb) { _error_callback = cb; }
    void SetCloseCallBack(const EventCallBack &cb) { _close_callback = cb; }
    void SetEventCallBack(const EventCallBack &cb) { _event_callback = cb; }

    // 当前是否监控了可读
    bool ReadAble() { return (_events & EPOLLIN); }

    // 当前是否监控了可写
    bool WriteAble() { return (_events & EPOLLOUT); }

    // 启动读事件监控
    void EnableRead() { _events |= EPOLLIN; }

    // 启动写事件监控
    void EnableWrite() { _events |= EPOLLOUT; }

    // 关闭读事件监控
    void DisableRead() { _events &= ~EPOLLIN; }

    // 关闭写事件监控
    void DisableWrite() { _events &= ~EPOLLOUT; }

    // 关闭所有事件监控
    void DisableAll()
    {
        _events = 0;
        Update();
    }

    // 从底层的Poller/EventLoop中移除当前Channel的监控
    void Remove();
    // 将当前Channel的_events配置状态更新到底层的Poller/EventLoop中
    void Update();

    void HandleEvent()
    {
        // 处理读事件（包括正常数据可读、对端关闭导致的读半闭、紧急带外数据）
        if ((_revents & EPOLLIN) || (_revents & EPOLLRDHUP) || (_revents & EPOLLPRI))
        {
            if (_read_callback)
                _read_callback();
        }

        /*有可能会释放连接的操作事件，一次只处理一个*/
        if (_revents & EPOLLOUT)
        { // 处理写事件
            if (_write_callback)
                _write_callback();
        }
        else if (_revents & EPOLLERR)
        { // 处理错误事件
            if (_error_callback)
                _error_callback();
        }
        else if (_revents & EPOLLHUP)
        { // 处理挂起事件（通常为连接断开）
            if (_close_callback)
                _close_callback();
        }
        // 只要有事件触发，最后都会调用的通用回调（可用于刷新连接活跃度等统一收尾操作）
        if (_event_callback)
            _event_callback();
    }
};

#define MAX_EPOLLEVENTS 1024
// Poller类：对 Linux epoll 机制的封装，负责底层的 I/O 多路复用事件监听
class Poller
{
private:
    int _epfd;                                    // epoll 实例的文件描述符
    struct epoll_event _evs[MAX_EPOLLEVENTS];     // 作为 epoll_wait 的传出数组，用于保存实际就绪的事件集合
    std::unordered_map<int, Channel *> _channels; // 管理当前 Poller 正在监控的所有 Channel，键为 fd，值为对应的 Channel 指针
private:
    // 对epoll的直接操作
    void Update(Channel *channel, int op)
    {
        // int epoll_ctl(int epfd,int op,int fd,struct epoll_event *ev)
        int fd = channel->Fd();
        struct epoll_event ev;
        ev.data.fd = fd;               // 设置底层 epoll 触发时返回的携带数据为 fd
        ev.events = channel->Events(); // 将 Channel 期望监控的事件掩码传递给 epoll
        int ret = epoll_ctl(_epfd, op, fd, &ev);
        if (ret < 0)
        {
            ERR_LOG("EPOLLCTL FAILED!");
        }
        return;
    }

    // 判断一个Channel是否已经添加了事件监控 (通过在哈希表中查找)
    bool HasChannel(Channel *channel)
    {
        auto it = _channels.find(channel->Fd());
        if (it == _channels.end())
            return false;
        return true;
    }

public:
    Poller()
    {
        _epfd = epoll_create(MAX_EPOLLEVENTS);
        if (_epfd < 0)
        {
            ERR_LOG("EPOLL CREATE FAILED!!");
            abort(); // 退出程序
        }
    }

    // 添加或修改监控事件(暴露给外部 EventLoop 调用的接口)
    void UpdateEvent(Channel *channel)
    {
        // 不存在则添加：先在本地哈希表中记录该 Channel，再向系统内核注册
        if (!HasChannel(channel))
        {
            _channels.insert(std::make_pair(channel->Fd(), channel));
            return Update(channel, EPOLL_CTL_ADD);
        }
        // 已经存在，说明是修改原有的监听事件 (如从只读修改为读+写)
        return Update(channel, EPOLL_CTL_MOD);
    }

    // 移除监控
    void RemoveEvent(Channel *channel)
    {
        auto it = _channels.find(channel->Fd());
        if (it != _channels.end())
        {
            _channels.erase(it);
        }
        Update(channel, EPOLL_CTL_DEL);
    }

    // 开始监控，返回活跃连接 (核心事件轮询函数)
    void Poll(std::vector<Channel *> *active)
    {
        // int epoll_wait(int epfd,struct epoll_event *evs,int maxevents,int timeout)
        //  阻塞等待事件发生，timeout 为 -1 表示死等，直到有事件或信号中断
        int nfds = epoll_wait(_epfd, _evs, MAX_EPOLLEVENTS, -1);
        if (nfds < 0)
        {
            // 如果是被系统信号中断 (EINTR)，属于正常现象，直接返回等待下次轮询
            if (errno == EINTR)
                return;
            ERR_LOG("EPOLL WAIT ERROR:%s\n", strerror(errno));
            abort();
        }

        // 遍历所有内核返回就绪事件
        for (int i = 0; i < nfds; i++)
        {
            // 根据底层返回的 fd，在本地哈希表中找到对应的 Channel 模块
            auto it = _channels.find(_evs[i].data.fd);
            assert(it != _channels.end());          // 断言一定能找到，保证内核通知的 fd 确实由我们在管理
            it->second->SetREvents(_evs[i].events); // 设置实际就绪的事件
            active->push_back(it->second);          // 将准备好的 Channel 添加到 active 列表中，交由上层统一执行回调逻辑
            return;
        }
    }
};

using TaskFunc = std::function<void()>;
using ReleaseFunc = std::function<void()>;

// TimerTask: 定时任务封装类
// 核心思想：利用 RAII 机制，在对象析构时执行定时任务
class TimerTask
{
private:
    uint64_t _id;         // 定时器对象全局唯一ID
    uint32_t _timeout;    // 定时任务的延迟时间（超时时间）
    bool _canceled;       // 任务取消标志：false-正常执行，true-被取消
    TaskFunc _task_cb;    // 真正要执行的定时任务回调函
    ReleaseFunc _release; // 释放回调：用于在析构时从 TimerWheel 中清理自身记录
public:
    TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb)
        : _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}
    ~TimerTask()
    {
        if (_canceled == false)
            _task_cb();
        _release();
    }
    void Cancel() { _canceled = true; }                       // 取消任务
    void SetRelease(const ReleaseFunc &cb) { _release = cb; } // 设置清理回调
    uint32_t DelayTime() { return _timeout; }                 // 获取延迟时间
};

// TimerWheel: 时间轮定时器模块
// 核心思想：环形数组 + tick秒针，利用 timerfd 实现按秒驱动
class EventLoop;
class TimerWheel
{
private:
    using WeakTask = std::weak_ptr<TimerTask>;
    using PtrTask = std::shared_ptr<TimerTask>;
    int _tick;                                      // 当前表盘的“秒针”位置，走到哪就释放并执行哪里的任务
    int _capacity;                                  // 表盘最大刻度数量（也是最大延迟时间，如 60 秒）
    std::vector<std::vector<PtrTask>> _wheel;       // 时间轮本体：每个刻度上挂载一个数组，保存该时刻要到期的所有定时任务
    std::unordered_map<uint64_t, WeakTask> _timers; // 快速查找映射表：用于刷新或取消任务时，通过 ID 快速找到任务（使用 weak_ptr 避免影响引用计数）
    EventLoop *_loop;                               // 绑定事件循环
    int _timerfd;                                   // 定时器文件描述符，由 Linux 内核提供定时通知
    std::unique_ptr<Channel> _timer_channel;        // 定时器事件通道
private:
    // 供 TimerTask 析构时调用的回调，将自身从映射表中抹除
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it != _timers.end())
        {
            _timers.erase(it);
        }
    }

    // 创建并配置 timerfd（每秒触发一次可读事件）
    static int CreateTimerFd()
    {
        int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (timerfd < 0)
        {
            ERR_LOG("TIMERFD CREATE FAILED!");
            abort();
        }
        struct itimerspec itime;
        itime.it_value.tv_sec = 1; // 首次超时时间：1s后
        itime.it_value.tv_nsec = 0;
        itime.it_interval.tv_sec = 1; // 后续触发间隔：每1s触发一次
        itime.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd, 0, &itime, nullptr);
        return timerfd;
    }

    // 读取timerfd，获取自上次读取后超时的次数
    int ReadTimeFd()
    {
        uint64_t times;
        int ret = read(_timerfd, &time, 8);
        if (ret < 0)
        {
            ERR_LOG("READ TIMEFD FAILED!");
            abort();
        }
        return times;
    }

    // 秒针向前走一步，并执行该刻度上的所有任务
    void RunTimerTask()
    {
        _tick = (_tick + 1) % _capacity;
        _wheel[_tick].clear();
    }

    // timerfd 触发可读事件时的回调函数
    void OnTime()
    {
        int times = ReadTimeFd();
        // 根据实际超时次数（防止阻塞导致错漏），推进秒针
        for (int i = 0; i < times; i++)
        {
            RunTimerTask();
        }
    }

    // 【线程安全保证】以下 InLoop 函数必须在 EventLoop 所在线程中执行
    void TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc &cb)
    {
        PtrTask pt(new TimerTask(id, delay, cb));
        pt->SetRelease(std::bind(&TimerWheel::RemoveTimer, this, id));
        int pos = (_tick + delay) % _capacity; // 计算在时间轮上的落点
        _wheel[pos].push_back(pt);             // 加入时间轮
        _timers[id] = WeakTask(pt);            // 加入快速查找表
    }

    void TimerRefreshInLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return;                     // 任务已过期或不存在
        PtrTask pt = it->second.lock(); // 提升为 shared_ptr

        if (pt)
        {
            int delay = pt->DelayTime();
            int pos = (_tick + delay) % _capacity; // 重新放入新的刻度位置（旧位置的指针依然存在，但在析构前由于 _canceled 的机制或多次引用的存在，不会被重复执行）
            _wheel[pos].push_back(pt);
        }
    }

    void TimerCancelInLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return;
        PtrTask pt = it->second.lock();
        if (pt)
            pt->Cancel(); // 设置取消标记，析构时便不再执行 task_cb
    }

public:
    TimerWheel(EventLoop *loop)
        : _capacity(60), _tick(0), _wheel(_capacity), _loop(loop), _timerfd(CreateTimerFd()), _timer_channel(new Channel(_loop, _timerfd))
    {
        _timer_channel->SetReadCallBack(std::bind(&TimerWheel::OnTime, this));
        _timer_channel->EnableRead(); // 启动读事件监控
    }

    // 外部调用的公开接口（内部通过 EventLoop 保证线程安全，需补充实现：调用 _loop->RunInLoop）
    void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb);
    void TimerRefresh(uint64_t id);
    void TimerCancel(uint64_t id);

    // 判断定时器是否存在（只能在 Loop 线程中调用）
    bool HasTimer(uint64_t id)
    {
        return _timers.find(id) != _timers.end();
    }
};

class EventLoop
{
private:
    using Functor = std::function<void()>;
    std::thread::id _thread_id;              // 记录当前 EventLoop 所属的线程 ID，用于判断是否在当前线程
    int _event_fd;                           // eventfd：用于跨线程唤醒阻塞在 epoll_wait 上的 EventLoop 线程
    std::unique_ptr<Channel> _event_channel; // 专门管理 eventfd 的事件通道
    Poller _poller;                          // I/O 多路复用器（如 epoll 封装），负责监听所有文件描述符的事件
    std::vector<Functor> _tasks;             // 任务池：存放其他线程要求本 EventLoop 执行的回调任务
    std::mutex _mutex;
    TimerWheel _timer_wheel;

public:
    void RunAllTask()
    {
        std::vector<Functor> functor;
        {
            // 将 _tasks 里的任务交换到本地的 functor 中，然后立刻释放锁。
            // 这样在执行任务时，不会阻塞其他线程继续向 _tasks 中投递新任务
            std::unique_lock<std::mutex> _lock(_mutex);
            _tasks.swap(functor);
        }
        for (auto &f : functor)
            f();
        return;
    }

    // 创建eventfd(内核提供事件等待与唤醒机制)
    static int CreateEventFd()
    {
        // EFD_CLOEXEC:fork子进程时关闭该fd
        // EFD_NONBLOCK:设置为非阻塞
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0)
        {
            ERR_LOG("CREATE EVENTFD FAILED!!");
            abort();
        }
        return efd;
    }

    // 处理 eventfd 的读事件（消费唤醒信号）
    void ReadEventFd()
    {
        uint64_t res = 0;
        // 读取 eventfd 中的值，每次读取会将内部计数器清零，从而消除可读事件
        int ret = read(_event_fd, &res, sizeof res);
        if (res < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                return;
            ERR_LOG("READ EVENTFD FAILED!");
            abort();
        }
        return;
    }

    // 唤醒 EventLoop（向 eventfd 写数据）
    void WakeUpEventFd()
    {
        uint64_t val = 1;
        int ret = write(_event_fd, &val, sizeof val);
        if (ret < 0)
        {
            if (errno == EINTR)
                return;
            ERR_LOG("WRITE EVENTFD FAILED!");
            abort();
        }
        return;
    }

public:
    // 构造函数：初始化各个组件，并启动对eventfd的监听
    EventLoop() : _thread_id(std::this_thread::get_id()), _event_fd(CreateEventFd()), _event_channel(new Channel(this, _event_fd)), _timer_wheel(this)
    {
        // 绑定eventfd的读回调，当有人调用WakeUpEventFd写数据时，此函数会被触发读取数据
        _event_channel->SetReadCallBack(std::bind(&EventLoop::ReadEventFd, this));
        // 将event注册到Poller中，开始监听它的可读事件
        _event_channel->EnableRead();
    }

    // 启动事件循环（核心驱动函数）
    // 经典的 Reactor 三步走：事件监控 -> 就绪事件处理 -> 执行任务
    void Start()
    {
        while (1)
        {
            // 1.事件监控：阻塞等待（或超时等待）文件描述符上有事件发生
            std::vector<Channel *> actives;
            _poller.Poll(&actives);
            // 2.事件处理：遍历所有触发事件的Channel，调用对应的读/写/错误回调
            for (auto &channel : actives)
            {
                channel->HandleEvent();
            }
            // 3. 执行任务：执行其他线程通过 QueueInLoop 投递过来的待办任务
            RunAllTask();
        }
    }

    // 判断当前代码是否运行在 EventLoop 专属的线程内
    bool IsInLoop()
    {
        return (_thread_id == std::this_thread::get_id());
    }

    // 断言检测：严格保证某段代码只能在当前 Loop 线程中执行
    void AssertInLoop()
    {
        assert(_thread_id == std::this_thread::get_id());
    }

    // 将任务压入任务池，并唤醒Loop线程
    void QueueInLoop(const Functor &cb)
    {
        {
            std::unique_lock<std::mutex> _lock(_mutex);
            _tasks.push_back(cb);
        }
        WakeUpEventFd();
    }

    // 核心任务调度：在Loop线程中执行指定任务
    void RunInLoop(const Functor &cb)
    {
        if (IsInLoop())
            return cb();        // 如果当前就是 Loop 线程，直接同步执行
        return QueueInLoop(cb); // 如果是其他线程调用的，则将任务放入队列，等待 Loop 线程自行执行
    }

    // 添加/修改描述符的事件监控
    void UpdateEvent(Channel *channel) { return _poller.UpdateEvent(channel); }

    // 移除描述符的监控
    void RemoveEvent(Channel *channel) { return _poller.RemoveEvent(channel); }

    // 内部时间轮接口代理
    void TimerAdd(uint64_t id,uint32_t delay,const TaskFunc &cb){
        return _timer_wheel.TimerAdd(id,delay,cb);
    }
    void TimerRefresh(uint64_t id){return _timer_wheel.TimerRefresh(id);}
    void TimerCancel(uint64_t id){return _timer_wheel.TimerCancel(id);}
    bool HasTimer(uint64_t id){return _timer_wheel.HasTimer(id);}
};
