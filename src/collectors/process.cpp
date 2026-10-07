#include "collectors/process.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sstream>

namespace whyhot {

ProcessCollector::ProcessCollector(int top_n) : top_n_(top_n) {
  long tck = sysconf(_SC_CLK_TCK);
  if (tck > 0) clk_tck_ = static_cast<double>(tck);
}

namespace {

// Parses /proc/<pid>/stat, which is tricky because the comm field (2nd
// field) is parenthesized and may itself contain spaces or parens.
bool parseProcStat(const std::string& path, std::string* comm,
                    uint64_t* utime, uint64_t* stime, long* rss_pages) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  if (!std::getline(f, line)) return false;

  const auto open_paren = line.find('(');
  const auto close_paren = line.rfind(')');
  if (open_paren == std::string::npos || close_paren == std::string::npos ||
      close_paren < open_paren) {
    return false;
  }
  *comm = line.substr(open_paren + 1, close_paren - open_paren - 1);

  std::istringstream rest(line.substr(close_paren + 1));
  std::string state;
  rest >> state;  // field 3 (state)
  long ppid, pgrp, session, tty_nr, tpgid;
  unsigned long flags, minflt, cminflt, majflt, cmajflt;
  unsigned long ut, st;
  rest >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags >> minflt >>
      cminflt >> majflt >> cmajflt >> ut >> st;
  if (!rest) return false;
  *utime = ut;
  *stime = st;

  // Skip cutime, cstime, priority, nice, num_threads, itrealvalue,
  // starttime, vsize to reach rss (24th field overall).
  long cutime, cstime, priority, nice_val, num_threads, itrealvalue;
  unsigned long long starttime;
  unsigned long vsize;
  long rss = 0;
  rest >> cutime >> cstime >> priority >> nice_val >> num_threads >>
      itrealvalue >> starttime >> vsize >> rss;
  *rss_pages = rest ? rss : 0;
  return true;
}

std::string readCmdline(const std::string& pid_dir) {
  std::ifstream f(pid_dir + "/cmdline", std::ios::binary);
  if (!f) return "";
  std::string raw((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  while (!raw.empty() && raw.back() == '\0') raw.pop_back();
  for (char& c : raw) {
    if (c == '\0') c = ' ';
  }
  return raw;
}

}  // namespace

ProcessCollector::Result ProcessCollector::sample() {
  Result result;
  const auto now = std::chrono::steady_clock::now();
  const double wall_seconds =
      first_sample_
          ? 0.0
          : std::chrono::duration<double>(now - prev_wall_).count();

  std::unordered_map<int, PrevInfo> current;
  const long page_kb = sysconf(_SC_PAGESIZE) / 1024;

  DIR* dir = opendir("/proc");
  if (!dir) return result;

  std::vector<ProcessInfo> all;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
      continue;
    }
    int pid = 0;
    if (std::from_chars(name.data(), name.data() + name.size(), pid).ec !=
        std::errc()) {
      continue;
    }

    std::string comm;
    uint64_t utime = 0, stime = 0;
    long rss_pages = 0;
    if (!parseProcStat("/proc/" + name + "/stat", &comm, &utime, &stime,
                        &rss_pages)) {
      continue;
    }
    const uint64_t total_ticks = utime + stime;
    current[pid] = {total_ticks, comm};

    ProcessInfo info;
    info.pid = pid;
    info.comm = comm;
    info.cmdline = readCmdline("/proc/" + name);
    if (info.cmdline.empty()) info.cmdline = comm;
    info.rss_kb = rss_pages * page_kb;

    auto it = prev_.find(pid);
    if (it != prev_.end() && wall_seconds > 0.0) {
      const uint64_t delta_ticks =
          total_ticks >= it->second.ticks ? total_ticks - it->second.ticks : 0;
      info.cpu_pct =
          100.0 * (static_cast<double>(delta_ticks) / clk_tck_) / wall_seconds;
    }
    all.push_back(info);

    if (it == prev_.end() && !first_sample_) {
      result.events.push_back({pid, comm, ProcessEvent::Type::kStart});
    }
  }
  closedir(dir);

  if (!first_sample_) {
    for (const auto& [pid, info] : prev_) {
      if (current.find(pid) == current.end()) {
        result.events.push_back({pid, info.comm, ProcessEvent::Type::kExit});
      }
    }
  }

  std::sort(all.begin(), all.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
    return a.cpu_pct > b.cpu_pct;
  });
  if (static_cast<int>(all.size()) > top_n_) all.resize(top_n_);
  result.top = std::move(all);

  prev_ = std::move(current);
  prev_wall_ = now;
  first_sample_ = false;
  return result;
}

}  // namespace whyhot
