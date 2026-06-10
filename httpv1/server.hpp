#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iostream>
#include <cstdint>

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
    void Read(void *buf, uint64_t len){
        assert(len<=ReadAbleSize());
        std::copy(ReadPosition(),ReadPosition()+len,(char*)buf);
    }

    // 读取数据到buf，并移动偏移量
    void ReadAndPop(void *buf,uint64_t len)
    {
        Read(buf,len);
        MoveReadOffset(len);
    }

    //读取数据作为std::string 返回
    std::string ReadAsString(uint64_t len) {
        assert(len<=ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0],len);
        return str;
    }

    //读取数据作为std::string 返回，并使用读偏移量
    std::string ReadAsStringAndPop(uint64_t len)
    {
        assert(len<=ReadAbleSize());
        std::string str = ReadAsString(len);
        MoveReadOffset(len);
        return str;
    }

    // 查找换行符 '\n'，常用于解决 TCP 粘包/半包问题中的按行读取
    char *FindCRLF()
    {
        char *res = (char*)memchr(ReadPosition(),'\n',ReadAbleSize());
        return res;
    }

    // 获取一行数据 (包括换行符)，但不移动偏移量
    std::string GetLine()
    {
        char *pos = FindCRLF();
        if(pos==nullptr)
        {
            return "";
        }
        return ReadAsString(pos-ReadPosition()+1);
    }

    //获取一行数据并移除
    std::string GetLineAndPop()
    {
        std::string str = GetLine();
        MoveReadOffset(str.size());
        return str;
    }

    // 清空缓冲区
    void Clear(){
        _reader_idx = 0;
        _writer_idx = 0;
    }
};