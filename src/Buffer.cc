#include "net/Buffer.h"

#include <sys/uio.h>

#include <algorithm>
#include <cerrno>

namespace net {

// 类内已给初始值，这里显式给出定义，避免某些编译单元取地址时链接失败
const size_t Buffer::kCheapPrepend;
const size_t Buffer::kInitialSize;

const char Buffer::kCRLF[] = "\r\n";

void Buffer::makeSpace(size_t len) {
    // 情况一：前面读走的空间 + 尾部空闲 >= 需要的长度，就原地搬移，
    // 不扩容（长连接反复读写时这条路径命中率极高）。
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        buffer_.resize(writerIndex_ + len);
    } else {
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];  // 栈上 64KB，远小于默认 8MB 栈，安全

    struct iovec vec[2];
    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    // 缓冲区自身剩余空间比 64KB 还大时，就不需要第二块了
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;

    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writerIndex_ += static_cast<size_t>(n);
    } else {
        // 读满了缓冲区还不够，多出来的在 extrabuf 里
        writerIndex_ = buffer_.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}

}  // namespace net
