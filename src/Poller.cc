#include "net/Poller.h"

#include "net/Channel.h"

namespace net {

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

}  // namespace net
