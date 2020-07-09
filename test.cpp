#include <string_view>
#include <string>
#include <iostream>
#include <thread>

#include <sys/epoll.h>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>

class ConnectionHandler : public AMQP::TcpHandler {
public:
  void monitor(AMQP::TcpConnection *connection, int fd, int flags) override {
    std::cout << "Monitor request from " << connection << "\n";
    std::cout << "Monitoring " << fd << " for "
              << (flags & AMQP::readable ? "reading " : "")
              << (flags & AMQP::writable ? "writing" : "") << "\n";
    int epoll_op = EPOLL_CTL_ADD;
    if (fdToConnection.count(fd)) {
      if (flags == 0) {
        if (epoll_ctl(watch_fd, EPOLL_CTL_DEL, fd, nullptr) < 0)
          std::cerr << "epoll_ctl() failed: " << strerror(errno) << "\n";
        else
          fdToConnection.erase(fd);
        return;
      } else
        epoll_op = EPOLL_CTL_MOD;
    } else if (flags == 0)
      return;
    epoll_event event;
    event.data.fd = fd;
    event.events = 0;
    if (flags & AMQP::readable)
      event.events |= EPOLLIN;
    if (flags & AMQP::writable)
      event.events |= EPOLLOUT;
    if (epoll_ctl(watch_fd, epoll_op, fd, &event) < 0)
      std::cerr << "epoll_ctl() failed: " << strerror(errno) << "\n";
    else
      fdToConnection[fd] = connection;
  }

  ~ConnectionHandler() {
    stop_token = true;
    poll_thread.join();
  }

  uint16_t onNegotiate(AMQP::TcpConnection *, uint16_t /*interval*/) {
    return 0;
  }

private:
  int watch_fd = epoll_create1(0);
  std::unordered_map<int, AMQP::TcpConnection *> fdToConnection;
  bool stop_token = false;
  std::thread poll_thread{[this] () {
    while(!stop_token) {
      std::array<epoll_event, 10> events;
      int poll_r = epoll_wait(watch_fd, events.data(), events.size(), -1);
      if (poll_r > 0) {
        for (size_t i = 0; i < poll_r; ++i) {
          const auto &e = events[i];
          auto connection = static_cast<AMQP::TcpConnection *>(e.data.ptr);
          int flags = 0;
          if (e.events & EPOLLIN)
            flags |= AMQP::readable;
          if (e.events & EPOLLOUT)
            flags |= AMQP::writable;
          if (flags != 0)
            fdToConnection.at(e.data.fd)->process(e.data.fd, flags);
        }
      } else if (poll_r < 0)
        std::cerr << "epoll_wait() failed: " << strerror(errno) << "\n";
    }
  }};
};

int main() {
  using namespace std::chrono_literals;
  ConnectionHandler handler;
  AMQP::TcpConnection connection{&handler, AMQP::Address{"amqp://guest:guest@localhost/"}};
  AMQP::TcpChannel channel{&connection};
  channel.onError([](const char *message) {
      std::cerr << "channel error: " << message << "\n";
    });
  channel.declareExchange("test-exchange", AMQP::fanout);
  channel.declareQueue(AMQP::exclusive).onSuccess(
    [&channel](const std::string &name, uint32_t messagecount, uint32_t consumercount) {
      channel.bindQueue("test-exchange", name, "add-reconstruction");
      channel.consume(name).onReceived(
          [&channel] (const AMQP::Message &message, uint64_t deliveryTag, bool redelivered) {
            std::cout << "Recieved message: " << std::string{message.body(), message.bodySize()} << "\n";
            channel.ack(deliveryTag);
          });
    });
  channel.publish("test-exchange", "add-reconstruction", "test message");
  std::this_thread::sleep_for(1min);
  return 0;
}
