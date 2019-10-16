/*
MIT License

Copyright (c) 2019 Meng Rao <raomeng1@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <string>
#include <string.h>

namespace admincmd {

template<uint32_t RecvBufSize>
class SocketTcpConnection
{
public:
  ~SocketTcpConnection() { close("destruct"); }

  const std::string& getLastError() { return last_error_; };

  bool isConnected() { return fd_ >= 0; }

  bool getPeername(struct sockaddr_in& addr) {
    socklen_t addr_len = sizeof(addr);
    return ::getpeername(fd_, (struct sockaddr*)&addr, &addr_len) == 0;
  }

  void close(const char* reason, bool check_errno = false) {
    if (fd_ >= 0) {
      saveError(reason, check_errno);
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool write(const char* data, uint32_t size, bool more = false) {
    int flags = MSG_NOSIGNAL;
    if (more) flags |= MSG_MORE;
    do {
      int sent = ::send(fd_, data, size, flags);
      if (sent < 0) {
        if (errno != EAGAIN) {
          close("send error", true);
          return false;
        }
        continue;
      }
      data += sent;
      size -= sent;
    } while (size != 0);
    return true;
  }

  template<typename Handler>
  bool read(Handler handler) {
    int ret = ::read(fd_, recvbuf_ + tail_, RecvBufSize - tail_);
    if (ret <= 0) {
      if (ret < 0 && errno == EAGAIN) return false;
      if (ret < 0) {
        close("read error", true);
      }
      else {
        close("remote close");
      }
      return false;
    }
    tail_ += ret;

    uint32_t remaining = handler(recvbuf_ + head_, tail_ - head_);
    if (remaining == 0) {
      head_ = tail_ = 0;
    }
    else {
      head_ = tail_ - remaining;
      if (head_ >= RecvBufSize / 2) {
        memcpy(recvbuf_, recvbuf_ + head_, remaining);
        head_ = 0;
        tail_ = remaining;
      }
      else if (tail_ == RecvBufSize) {
        close("recv buf full");
      }
    }
    return true;
  }

protected:
  template<uint32_t>
  friend class SocketTcpServer;

  bool open(int fd) {
    fd_ = fd;
    head_ = tail_ = 0;

    int flags = fcntl(fd_, F_GETFL, 0);
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error", true);
      return false;
    }

    int yes = 1;
    if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
      close("setsockopt TCP_NODELAY error", true);
      return false;
    }

    return true;
  }

  void saveError(const char* msg, bool check_errno) {
    last_error_ = msg;
    if (check_errno) {
      last_error_ += ": ";
      last_error_ += strerror(errno);
    }
  }

  int fd_ = -1;
  uint32_t head_;
  uint32_t tail_;
  char recvbuf_[RecvBufSize];
  std::string last_error_;
};

template<uint32_t RecvBufSize = 4096>
class SocketTcpServer
{
public:
  using TcpConnection = SocketTcpConnection<RecvBufSize>;

  bool init(const std::string& interface_name, const std::string& server_ip, uint16_t server_port) {
    listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd_ < 0) {
      saveError("socket error");
      return false;
    }

    int flags = fcntl(listenfd_, F_GETFL, 0);
    if (fcntl(listenfd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error");
      return false;
    }

    int yes = 1;
    if (setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
      close("setsockopt SO_REUSEADDR error");
      return false;
    }

    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip.data(), &(local_addr.sin_addr));
    local_addr.sin_port = htons(server_port);
    bzero(&(local_addr.sin_zero), 8);
    if (bind(listenfd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
      close("bind error");
      return false;
    }
    if (listen(listenfd_, 5) < 0) {
      close("listen error");
      return false;
    }

    return true;
  };

  void close(const char* reason) {
    if (listenfd_ >= 0) {
      saveError(reason);
      ::close(listenfd_);
      listenfd_ = -1;
    }
  }

  const std::string& getLastError() { return last_error_; };

  ~SocketTcpServer() { close("destruct"); }

  bool accept2(TcpConnection& conn) {
    struct sockaddr_in clientaddr;
    socklen_t addr_len = sizeof(clientaddr);
    int fd = ::accept(listenfd_, (struct sockaddr*)&(clientaddr), &addr_len);
    if (fd < 0) {
      return false;
    }
    if (!conn.open(fd)) {
      return false;
    }
    return true;
  }

private:
  void saveError(const char* msg) {
    last_error_ = msg;
    last_error_ += ": ";
    last_error_ += strerror(errno);
  }

  int listenfd_ = -1;
  std::string last_error_;
};

template<typename EventHandler, typename ConnUserData = char, uint32_t MaxConns = 10, uint32_t MaxCMDLen = 4096,
         uint64_t ConnTimeout = 0>
class AdminCMDServer
{
public:
  using TcpServer = SocketTcpServer<MaxCMDLen>;

  class Connection
  {
  public:
    ConnUserData user_data;

    // return true on success
    bool getPeername(struct sockaddr_in& addr) { return conn.getPeername(addr); }

    // more = true: MSG_MORE flag is set on this write
    // return true on if on success
    bool write(const char* data, uint32_t size, bool more = false) { return conn.write(data, size, more); }

    void close(const char* reason = "user close") { conn.close(reason); }

  private:
    friend class AdminCMDServer<EventHandler, ConnUserData, MaxConns>;

    uint64_t active_ts = 0;
    typename TcpServer::TcpConnection conn;
  };

  AdminCMDServer() {
    for (int i = 0; i < MaxConns; i++) {
      conns_[i] = conns_data_ + i;
    }
  }

  bool init(EventHandler* handler, const std::string& server_ip, uint16_t server_port) {
    handler_ = handler;
    return server_.init("", server_ip, server_port);
  }

  const std::string& getLastError() { return server_.getLastError(); }

  void poll(uint64_t now = 0) {
    if (conns_cnt_ < MaxConns) {
      Connection& new_conn = *conns_[conns_cnt_];
      if (server_.accept2(new_conn.conn)) {
        new_conn.active_ts = now;
        handler_->onAdminConnect(new_conn);
        conns_cnt_++;
      }
    }
    for (int i = 0; i < conns_cnt_;) {
      Connection& conn = *conns_[i];
      if (conn.conn.read([&](const char* data, uint32_t size) {
            const char* data_end = data + size;
            char buf[MaxCMDLen] = {0};
            const char* argv[MaxCMDLen];
            char* out = buf + 1;
            int argc = 0;
            bool in_quote = false;
            bool single_quote = false;
            while (data < data_end) {
              char ch = *data++;
              if (!in_quote) {
                if (ch == ' ' || ch == '\r')
                  *out++ = 0;
                else if (ch == '\n') {
                  if (argc) {
                    *out = 0;
                    handler_->onAdminCMD(conn, argc, argv);
                  }
                  out = buf + 1;
                  argc = 0;
                  in_quote = false;
                  size = data_end - data;
                }
                else {
                  if (*(out - 1) == 0) argv[argc++] = out;
                  if (ch == '\'')
                    in_quote = single_quote = true;
                  else if (ch == '"')
                    in_quote = true;
                  else if (ch == '\\')
                    *out++ = *data++;
                  else
                    *out++ = ch;
                }
              }
              else {
                if (single_quote) {
                  if (ch == '\'')
                    in_quote = single_quote = false;
                  else
                    *out++ = ch;
                }
                else {
                  if (ch == '"') in_quote = false;
                  else if (ch == '\\' && (*data == '\\' || *data == '"'))
                    *out++ = *data++;
                  else
                    *out++ = ch;
                }
              }
            }
            return size;
          })) {
        conn.active_ts = now;
      }
      if (ConnTimeout > 0 && now > conn.active_ts + ConnTimeout) conn.conn.close("timeout");
      if (conn.conn.isConnected())
        i++;
      else {
        handler_->onAdminDisconnect(conn, conn.conn.getLastError());
        std::swap(conns_[i], conns_[--conns_cnt_]);
      }
    }
  }

private:
  EventHandler* handler_ = nullptr;
  TcpServer server_;

  uint32_t conns_cnt_ = 0;
  Connection* conns_[MaxConns];
  Connection conns_data_[MaxConns];
};

} // namespace admincmd
