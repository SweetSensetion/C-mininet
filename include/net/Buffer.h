// Buffer.h -- 应用层缓冲区
//
// 为什么必须有应用层缓冲区（内核已经有 socket buffer 了）：
//   1. 非阻塞 read 只能读到"当前可读的部分"，一条完整消息可能只到了一半，
//      剩下的必须存在用户态，等下次事件到来再拼——这是拆包的前提；
//   2. 非阻塞 write 可能只写出去一部分，剩余数据必须缓存，等 EPOLLOUT
//      再继续发——否则业务数据就丢了；
//   3. 内核 buffer 大小不可控且不可查询，业务无法在用户态做流控。
//
// 内存布局：
//   +-------------------+------------------+------------------+
//   | prependable bytes |  readable bytes  |  writable bytes  |
//   |    (readerIndex)  |                  |  (writerIndex)   |
//   0                   readerIndex        writerIndex        size
//
//   read   -> writerIndex 增加
//   retrieve -> readerIndex 增加（不移动数据）
//   读空后两个索引复位到 kCheapPrepend，避免反复分配。
#ifndef MINET_BUFFER_H
#define MINET_BUFFER_H

#include <arpa/inet.h>
#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace net {

class Buffer {
public:
    static const size_t kCheapPrepend = 8;   // 预留给协议头，编码时可就地插入
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }

    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    void hasWritten(size_t len) { writerIndex_ += len; }

    void retrieve(size_t len) {
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    std::string retrieveAsString(size_t len) {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::memcpy(beginWrite(), data, len);
        hasWritten(len);
    }

    void append(const void* data, size_t len) {
        append(static_cast<const char*>(data), len);
    }

    void append(const std::string& str) {
        append(str.data(), str.size());
    }

    // 大端写入 32 位整数（协议头用）
    void appendInt32(uint32_t x) {
        uint32_t be = htonl(x);
        append(&be, sizeof be);
    }

    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    // 从内核读数据。
    // 用 readv 一次读进"缓冲区剩余空间 + 栈上 64KB 溢出处"两块内存：
    //   * 缓冲区够大时只用一个 iovec，一次拷贝；
    //   * 缓冲区不够时溢出部分直接进栈，避免为一次突发流量反复 resize。
    ssize_t readFd(int fd, int* savedErrno);

    void swap(Buffer& rhs) {
        buffer_.swap(rhs.buffer_);
        std::swap(readerIndex_, rhs.readerIndex_);
        std::swap(writerIndex_, rhs.writerIndex_);
    }

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }

    void makeSpace(size_t len);

    static const char kCRLF[];

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

}  // namespace net

#endif  // MINET_BUFFER_H
