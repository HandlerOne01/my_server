#include "server.hpp"
#include <cassert>
#include <iostream>

int main() {
    // 1. 基本读写
    Buffer buf;
    buf.WriteAndPush("hello", 5);
    assert(buf.ReadAbleSize() == 5);
    assert(buf.ReadAsStringAndPop(5) == "hello");
    assert(buf.ReadAbleSize() == 0);
    std::cout << "[PASS] 基本读写" << std::endl;

    // 2. 偏移量
    buf.Clear();
    buf.WriteAndPush("abc", 3);
    assert(buf.TailIdleSize() == BUFFER_DEFAULT_SIZE - 3);
    assert(buf.HandIdleSize() == 0);
    char tmp;
    buf.ReadAndPop(&tmp, 1);  // 读走一个字节
    assert(buf.ReadAbleSize() == 2);
    assert(buf.HandIdleSize() == 1);
    buf.Clear();
    std::cout << "[PASS] 偏移量" << std::endl;

    // 3. 触发扩容 (写满后再写1字节)
    buf.Clear();
    std::string big(BUFFER_DEFAULT_SIZE, 'x');
    buf.WriteAndPush(big.c_str(), big.size());
    buf.WriteAndPush("y", 1);  // 触发 resize
    assert(buf.ReadAbleSize() == BUFFER_DEFAULT_SIZE + 1);
    buf.Clear();
    std::cout << "[PASS] 扩容" << std::endl;

    // 4. 行读取
    Buffer buf2;
    buf2.WriteAndPush("GET / HTTP/1.1\r\n", 16);
    assert(buf2.FindCRLF() != nullptr);
    assert(buf2.GetLineAndPop() == "GET / HTTP/1.1\r\n");
    assert(buf2.ReadAbleSize() == 0);
    std::cout << "[PASS] 行读取" << std::endl;

    // 5. 找不到换行符
    Buffer buf3;
    buf3.WriteAndPush("no_newline_here", 15);
    assert(buf3.FindCRLF() == nullptr);
    assert(buf3.GetLine() == "");
    std::cout << "[PASS] 无换行符" << std::endl;

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
