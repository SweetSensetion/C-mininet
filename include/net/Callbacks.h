// Callbacks.h -- 回调类型集中定义
//
// 把所有 std::function 类型别名集中在一处，好处是：
//   1. 修改签名只改一个地方，编译器会在所有赋值点报错；
//   2. 阅读 TcpConnection 时不会被一长串模板参数打断。
#ifndef MINET_CALLBACKS_H
#define MINET_CALLBACKS_H

#include <functional>
#include <memory>

#include "net/Timestamp.h"

namespace net {

class Buffer;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// 连接建立 / 断开
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
// 连接销毁（库内部使用，业务不应设置）
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
// 发送缓冲区清空（可用于流控：发送完一批再读下一批）
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
// 发送缓冲区超过水位（背压：业务应暂停生产）
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;
// 收到数据：连接 + 输入缓冲 + 到达时间
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;
// 定时器
using TimerCallback = std::function<void()>;

}  // namespace net

#endif  // MINET_CALLBACKS_H
