#include "../server.hpp"

// 业务层：一个简单的Echo服务器实现
class EchoServer
{
private:
    TcpServer _server;

private:
    // 新连接建立时的回调函数
    void OnConnected(const PtrConnection &conn)
    {
        DBG_LOG("NEW CONNECTION:%p", conn.get()); // 仅打印新连接地址
    }

    // 连接关闭时的回调函数
    void OnClosed(const PtrConnection &conn)
    {
        DBG_LOG("CLOSE CONNECTION:%p", conn.get());
    }

    // 收到消息时的回调函数（Echo 的核心）
    void OnMessage(const PtrConnection &conn, Buffer *buf)
    {
        conn->Send(buf->ReadPosition(), buf->ReadAbleSize());
        buf->MoveReadOffset(buf->ReadAbleSize());
        conn->ShutDown();
    }

public:
    EchoServer(int port) : _server(port)
    {
        _server.SetThreadCount(2);
        _server.EnableInactiveRelease(10);

        _server.SetClosedCallBack(std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
        _server.SetConnectedCallBack(std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
        _server.SetMessageCallBack(std::bind(&EchoServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
    }

    void Start()
    {
        _server.Start();
    }
};