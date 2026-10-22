// Acceptor.h -- 只负责"接受连接"
//
// 把 accept 从 TcpServer 里拆出来，是为了让 mainReactor 的职责单一：
// 它只有 listen fd 一个 Channel，永远不碰业务数据。
#ifndef MINET_ACCEPTOR_H
#define MINET_ACCEPTOR_H

#include <functional>

#include "net/Channel.h"
#include "net/InetAddress.h"
#include "net/Socket.h"

namespace net {

class EventLoop;

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(const NewConnectionCallback& cb) { newConnectionCallback_ = cb; }

    bool listening() const { return listening_; }
    void listen();

private:
    void handleRead();

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    int idleFd_;   // EMFILE 时用来"腾一个 fd"的保留 fd
};

}  // namespace net

#endif  // MINET_ACCEPTOR_H
