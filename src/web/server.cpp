#include "web/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

#include "web/page.h"

namespace whyhot {

namespace {

struct Request {
  std::string method;
  std::string path;
};

// Reads and parses just the request line; we don't care about headers or
// bodies since every route here is a parameterless GET.
bool readRequestLine(int fd, Request* req) {
  std::string buf;
  buf.reserve(512);
  char c;
  // Read until the end of the request line, bounded so a malformed client
  // can't make us buffer forever.
  while (buf.size() < 4096) {
    const ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0) return false;
    if (c == '\n') break;
    if (c != '\r') buf += c;
  }
  std::istringstream iss(buf);
  std::string version;
  if (!(iss >> req->method >> req->path >> version)) return false;
  return true;
}

// A single send() call is only guaranteed to accept as much as fits in the
// socket's send buffer right now - for a response bigger than that (the
// JSON state payload regularly is, once a few incidents have long
// timelines), the rest was silently dropped, truncating the response and
// leaving clients with invalid JSON. Loop until every byte is actually
// sent, or the connection fails.
bool sendAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

void sendResponse(int fd, int status, const char* status_text, const char* content_type,
                   const std::string& body) {
  std::ostringstream head;
  head << "HTTP/1.1 " << status << " " << status_text << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n";
  const std::string head_str = head.str();
  if (!sendAll(fd, head_str.data(), head_str.size())) return;
  if (!body.empty()) sendAll(fd, body.data(), body.size());
}

}  // namespace

WebServer::WebServer(int port) : port_(port) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;

  int opt = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // localhost only, deliberately

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_ = true;
  accept_thread_ = std::thread(&WebServer::acceptLoop, this);
  return true;
}

void WebServer::stop() {
  if (!running_) return;
  running_ = false;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
}

void WebServer::updateSnapshot(std::string json) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_json_ = std::move(json);
}

std::string WebServer::currentSnapshot() {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_json_;
}

void WebServer::acceptLoop() {
  while (running_) {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    const int client_fd =
        ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) {
      if (!running_) break;
      continue;
    }
    // Requests are infrequent (one poll every ~2s from one browser tab), so
    // a thread per connection is simpler than a pool and costs nothing here.
    std::thread(&WebServer::handleConnection, this, client_fd).detach();
  }
}

void WebServer::handleConnection(int client_fd) {
  timeval tv{3, 0};
  ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  Request req;
  if (readRequestLine(client_fd, &req) && req.method == "GET") {
    if (req.path == "/") {
      sendResponse(client_fd, 200, "OK", "text/html; charset=utf-8", kIndexHtml);
    } else if (req.path == "/api/state") {
      sendResponse(client_fd, 200, "OK", "application/json", currentSnapshot());
    } else {
      sendResponse(client_fd, 404, "Not Found", "text/plain", "not found");
    }
  }
  ::close(client_fd);
}

}  // namespace whyhot
