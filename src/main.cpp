#include <fmt/core.h>
#include <cstdio>
#include <fmt/std.h>
#include <atomic>
#include <string>
#include <map>
#include <optional>
#include <vector>
#include <thread>
#include <string_view>
#include <sstream>

#include <cstdint>
#include <signal.h>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>

struct http_version {
  int major;
  int minor;
};

enum http_method : uint8_t {
  HTTP_METHOD_INVALID = 0,
  HTTP_METHOD_GET = 1,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE
};

struct http_request {
  http_version version;
  std::string path;
  http_method method;
  std::map<std::string, std::string> headers;
};

static std::atomic<bool> signal_ctrlc = false;

void sighandler_sigint(int sig) {
  signal_ctrlc = true;
}

int main(const int argc, const char* argv[]) {

  auto server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(server_fd == -1) {
    fmt::print(stderr, "err: Failed to create TCP socket: {}\n", errno);
    return 2;
  }

  {
    auto flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  }

  constexpr auto server_address = "0.0.0.0";
  constexpr auto server_port = 8080;
  sockaddr_in server_addr;
  server_addr.sin_addr.s_addr = inet_addr(server_address);
  server_addr.sin_port = htons(server_port);
  server_addr.sin_family = AF_INET;

  if(bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) ==
     -1) {
    fmt::print(
        stderr, "err: Could not bind to {}:{} {}\n", server_address, server_port, errno);
    close(server_fd);
    return 2;
  }

  if(listen(server_fd, 255) == -1) {

    fmt::print(
        stderr, "err: Could not listen on {}:{} {}\n", server_address, server_port,
        errno);
    close(server_fd);
  }

  auto epoll = epoll_create1(0);
  {
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll, EPOLL_CTL_ADD, server_fd, &ev);
  }

  struct sigaction sigold_sigint;
  struct sigaction sig_sigint;
  sig_sigint.sa_handler = sighandler_sigint;

  sigaction(SIGINT, &sig_sigint, &sigold_sigint);

  std::string_view response = "HTTP/1.0 200 Ok\r\n"
                              "Content-Type: text/plain\r\n"
                              "\r\n"
                              "\r\n"
                              "Hello World";

  fmt::print(stderr, "log: Listening on {}:{}\n", server_address, server_port);

  epoll_event epoll_events[1000];
  std::map<int, std::optional<std::string>> client_fds;

  while(!signal_ctrlc) {
    int wait_res = epoll_wait(epoll, epoll_events, 1000, 500);
    if(wait_res == -1) {
      if(errno == EINTR) {
        continue;
      }
    }
    for(int i = 0; i < wait_res; i++) {
      auto& event = epoll_events[i];
      int fd = event.data.fd;
      if(fd == server_fd) {
        // accept new connection
        auto client_fd = accept(server_fd, nullptr, 0);

        auto flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        fmt::print(stderr, "log: New connection\n");
        client_fds.insert({client_fd, std::nullopt});

        epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP;
        ev.data.fd = client_fd;
        epoll_ctl(epoll, EPOLL_CTL_ADD, client_fd, &ev);

        continue;
      }
      if(event.events & EPOLLIN) {
        char buffer[1024];
        int retry = 0;
        std::string request{};
        request.reserve(1024);
        auto it = client_fds.find(fd);
        while(true) {
          int recv_bytes = recv(fd, buffer, 1024, 0);
          if(recv_bytes == 0) {
            epoll_ctl(epoll, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            if(it != client_fds.end()) {
              client_fds.erase(it);
            }
            break;
          }
          if(recv_bytes == -1) {
            if(errno == EWOULDBLOCK || errno == EAGAIN) {
              retry++;
              if(retry == 10) {
                break;
              }
              continue;
            }
            if(errno == ECONNRESET) {
              fmt::print(stderr, "debug: fd={} ECONNRESET\n", fd);
            }
            epoll_ctl(epoll, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            if(it != client_fds.end()) {
              client_fds.erase(it);
            }
            break;
          }
          retry = 0;
          request.append(buffer, recv_bytes);
        }
        epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &ev);
        it->second = request;
      }
      if(event.events & EPOLLOUT) {

        // epoll_event ev;
        // ev.events = EPOLLIN;
        // ev.data.fd = fd;
        // epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &ev);

        auto request = client_fds.at(fd);
        if(!request.has_value()) {
          fmt::print(stderr, "err: FD available for write without request\n");
          continue;
        }

        std::stringstream stream(request.value());
        std::string method;
        std::string path;

        stream >> method >> path;

        if(method == "GET") {
          if(path == "/") {
            const char* buf = response.data();
            int bytes_left = response.size();

            while(bytes_left != 0) {
              int sent_bytes = send(fd, buf, bytes_left, 0);
              if(sent_bytes == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                  std::this_thread::yield();
                  continue;
                }
              }
              bytes_left -= sent_bytes;
              buf += sent_bytes;
            }
          } else {

            const char* buf = "HTTP/1.0 404 Not Found\r\n"
                              "Content-Type: text/plain\r\n"
                              "\r\n404";
            int bytes_left = strlen(buf);

            while(bytes_left != 0) {
              int sent_bytes = send(fd, buf, bytes_left, 0);
              if(sent_bytes == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                  std::this_thread::yield();
                  continue;
                }
              }
              bytes_left -= sent_bytes;
              buf += sent_bytes;
            }
          }
        }

        epoll_ctl(epoll, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        auto it = client_fds.find(fd);
        if(it != client_fds.end()) {
          client_fds.erase(it);
        }
      }
      if(event.events & EPOLLHUP) {
        epoll_ctl(epoll, EPOLL_CTL_DEL, fd, nullptr);
        auto it = client_fds.find(fd);
        if(it != client_fds.end()) {
          close(fd);
          client_fds.erase(it);
        }
      }
    }
  }
  fmt::print(stderr, "log: Shutting down\n");

  sigaction(SIGINT, &sigold_sigint, nullptr);

  for(auto& [fd, request]: client_fds) {
    close(fd);
  }

  close(epoll);
  close(server_fd);
  return 0;
}
