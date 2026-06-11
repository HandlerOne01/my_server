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
    int Fd() { return _fd;}

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
