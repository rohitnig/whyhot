#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace whyhot {

// A deliberately tiny HTTP/1.1 server: GET-only, no keep-alive, no
// threading pool, one short-lived thread per connection. It exists to
// serve two things - the embedded single-page UI, and a JSON snapshot
// the page polls every couple of seconds - so anything fancier here would
// be work the tool doesn't need. Binds to 127.0.0.1 only.
class WebServer {
 public:
  explicit WebServer(int port);
  ~WebServer();

  bool start();  // returns false if the port couldn't be bound
  void stop();

  void updateSnapshot(std::string json);

 private:
  int port_;
  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::atomic<bool> running_{false};

  std::mutex snapshot_mutex_;
  std::string snapshot_json_ = "{}";

  void acceptLoop();
  void handleConnection(int client_fd);
  std::string currentSnapshot();
};

}  // namespace whyhot
