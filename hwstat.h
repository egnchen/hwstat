#ifndef _HWSTAT_H
#define _HWSTAT_H

#include <cassert>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <spdlog/spdlog.h>

/** disable all stats */
// #define NO_STAT

/** use `rdtscp` instructin instead of `rdtsc`
 * `rdtscp` will flush the processor pipeline before taking the time while `rdtsc` would not. This
 * would introduce a little more overhead than `rdtsc`, but more accurate results.
 */
// #define USE_RDTSCP

/** predefine frequency of `rdtsc` instruction
 * if not defined, the frequency will be measured automatically everytime the program starts(takes
 * about 10ms)
 */
// #define TSC_FREQ_GHZ 2.3

namespace hwstat {

template <typename T>
struct GlobalStat {
  const char *name;
  const char *desc;
  std::mutex mtx;
  std::set<T *> instances;
  typename T::AggregateType agg;
  GlobalStat(const char *name, const char *desc = "") : name(name), desc(desc) {
    assert(name);
    std::lock_guard<std::mutex> guard(gMtx);
    stats.emplace(name, this);
  }
  ~GlobalStat() {
    std::lock_guard<std::mutex> guard(gMtx);
    stats.erase(name);
  }
  GlobalStat(const GlobalStat &) = delete;
  GlobalStat(GlobalStat &&) = delete;
  void reg(T *instance) {
    std::lock_guard<std::mutex> guard(mtx);
    instances.insert(instance);
  }
  void dereg(T *instance) {
    std::lock_guard<std::mutex> guard(mtx);
    agg = instance->aggregate(agg);
    instances.erase(instance);
  }
  typename T::AggregateType calcStat() {
    auto nagg = agg;
    std::lock_guard<std::mutex> guard(mtx);
    for (auto i : instances) {
      nagg = i->aggregate(nagg);
    }
    return nagg;
  }
  static void printStats();

private:
  static inline std::map<const char *, GlobalStat *> stats;
  static inline std::mutex gMtx;
};

struct SimpleStat {
  using CallbackType = std::function<std::string(void)>;
  const char *name;
  CallbackType callback;
  const char *desc;
  SimpleStat(const char *name, CallbackType cb, const char *desc = "")
      : name(name), callback(cb), desc(desc) {
    std::lock_guard<std::mutex> guard(gMtx);
    stats.insert({name, this});
  }
  SimpleStat(const SimpleStat &) = delete;
  SimpleStat(SimpleStat &&) = delete;
  ~SimpleStat() {
    std::lock_guard<std::mutex> guard(gMtx);
    stats.erase(name);
  }
  static void printStats();

private:
  static inline std::map<const char *, SimpleStat *> stats;
  static inline std::mutex gMtx;
};

struct RdtscTimerFunc {
  uint64_t operator()() {
    uint64_t a, d;
    asm volatile("rdtsc" : "=a"(a), "=d"(d));
    return a | (d << 32);
  }
};

struct RdtscpTimerFunc {
  uint64_t operator()() {
    uint64_t a, d;
    asm volatile("rdtscp" : "=a"(a), "=d"(d));
    return a | (d << 32);
  }
};

static inline double measure_tsc_ghz(int sleep_ms = 10) {
#ifdef NO_STAT
  return 0.0;
#endif
#ifdef TSC_FREQ_GHZ
  spdlog::info("predefined tsc frequency as {:.3}Ghz", TSC_FREQ_GHZ);
  return TSC_FREQ_GHZ;
#else
  auto timer_func = RdtscTimerFunc{};
  auto start_clk = std::chrono::high_resolution_clock::now();
  unsigned long start = timer_func();
  std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  auto end_clk = std::chrono::high_resolution_clock::now();
  unsigned long end = timer_func();
  auto count_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_clk - start_clk).count();
  auto ret = double(end - start) / count_ns;
  spdlog::info("measured tsc frequency as {:.3}Ghz", ret);
  return ret;
#endif
}

struct TimerAgg {
  uint64_t cnt = 0;
  uint64_t cycles = 0;
  static inline double kFreqGhz = measure_tsc_ghz();
  double getNanos() const { return cycles / kFreqGhz; }
  uint64_t getAvgCycles() const { return cycles / cnt; }
  double getAvgNanos() const { return getNanos() / cnt; }
};

struct PerThreadTimer {
  using GlobalTimer = GlobalStat<PerThreadTimer>;
  using AggregateType = TimerAgg;
  uint64_t cycles = 0;
  uint64_t cnt = 0;
  GlobalTimer *global_timer;
  PerThreadTimer(GlobalTimer *globalTimer) : global_timer(globalTimer) { globalTimer->reg(this); }
  PerThreadTimer(const PerThreadTimer &) = delete;
  PerThreadTimer(PerThreadTimer &&) = delete;
  ~PerThreadTimer() { global_timer->dereg(this); }
  void add(uint64_t dc = 0) {
    cycles += dc;
    cnt++;
  }
  AggregateType aggregate(AggregateType prev) {
    prev.cnt += cnt;
    prev.cycles += cycles;
    return prev;
  }
  AggregateType stat() { return global_timer->calcStat(); }
};

struct NoopTimer {
  using GlobalTimer = GlobalStat<NoopTimer>;
  using AggregateType = TimerAgg;
  NoopTimer(GlobalTimer *timer) {}
  NoopTimer(const NoopTimer &) = delete;
  NoopTimer(NoopTimer &&) = delete;
  ~NoopTimer() {}
  void add(uint64_t dc = 0) {}
  AggregateType aggregate(AggregateType prev) { return AggregateType{}; }
  AggregateType stat() { return AggregateType{}; }
};

struct PerThreadCounter {
  using GlobalCounter = GlobalStat<PerThreadCounter>;
  using AggregateType = uint64_t;
  uint64_t cnt = 0;
  GlobalCounter *global_counter;
  PerThreadCounter(GlobalCounter *globalCounter) : global_counter(globalCounter) {
    globalCounter->reg(this);
  }
  PerThreadCounter(const PerThreadCounter &) = delete;
  PerThreadCounter(PerThreadTimer &&) = delete;
  ~PerThreadCounter() { global_counter->dereg(this); }
  void add(int d = 1) { cnt += d; }
  uint64_t operator++() { return ++cnt; }
  uint64_t operator++(int) { return cnt++; }
  uint64_t operator+=(uint64_t d) { return cnt += d; }
  AggregateType aggregate(AggregateType prev) { return prev + cnt; }
  AggregateType stat() { return global_counter->calcStat(); }
};

struct NoopCounter {
  using GlobalCounter = GlobalStat<NoopCounter>;
  using AggregateType = uint64_t;
  NoopCounter(GlobalCounter *globalCounter) {}
  NoopCounter(const NoopCounter &) = delete;
  NoopCounter(NoopCounter &&) = delete;
  ~NoopCounter() {}
  void add(int d = 1) {}
  uint64_t operator++() { return 0; }
  uint64_t operator++(int) { return 0; }
  uint64_t operator+=(uint64_t d) { return 0; }
  AggregateType aggregate(AggregateType prev) { return AggregateType{}; }
  AggregateType stat() { return AggregateType{}; }
};

#ifndef NO_STAT
using CounterType = PerThreadCounter;
using TimerType = PerThreadTimer;
#else
using CounterType = NoopCounter;
using TimerType = NoopTimer;
#endif

template <typename TimerFunc = RdtscTimerFunc>
class StopwatchBase {
  TimerType &timer;
  TimerFunc timer_func;
  uint64_t st;
  uint64_t agg = 0;

public:
  StopwatchBase(TimerType &timer) : timer(timer), timer_func{} { restart(); }
  void pause() { agg += timer_func() - st; }
  void resume() { st = timer_func(); }
  void restart() { resume(); }
  void stop() {
    pause();
    timer.add(agg);
    agg = 0;
  }
};

class NoopStopwatch {
public:
  NoopStopwatch(TimerType &timer) {}
  void pause() {}
  void resume() {}
  void restart() {}
  void stop() {}
};

#ifdef NO_STAT
using Stopwatch = NoopStopwatch;
#else
#ifdef USE_RDTSCP
using Stopwatch = StopwatchBase<RdtscpTimerFunc>;
#else
using Stopwatch = StopwatchBase<RdtscTimerFunc>;
#endif
#endif

class ScopedTimer {
  Stopwatch sw;

public:
  ScopedTimer(TimerType &timer) : sw(timer) {}
  ~ScopedTimer() { sw.stop(); }
};

static inline std::string format_time(double nanos) {
  constexpr const char *units[] = {"ns", "us", "ms", "s"};
  int idx = 0;
  while (nanos >= 1000 && idx < 3) {
    nanos /= 1000;
    idx++;
  }
  return fmt::format("{:.3}{}", nanos, units[idx]);
}

template <typename T>
static inline size_t get_max_strlen(const std::map<const char *, T *> &stats) {
  size_t ret = 0;
  for (const auto kv : stats) {
    ret = std::max(ret, strlen(kv.first));
  }
  return ret;
}

template <>
inline void GlobalStat<PerThreadTimer>::printStats() {
  if (stats.size() == 0) {
    spdlog::info("NO TIMERS");
    return;
  }
  auto l = std::max(8UL, get_max_strlen(stats) + 2);
  spdlog::info("======TIMERS(freq = {:.3}Ghz)======", TimerAgg::kFreqGhz);
  spdlog::info("{:<{}}TIME\tCOUNT\tAVERAGE\t\tDESCRIPTION", "NAME", l);
  for (const auto kv : stats) {
    auto timer = kv.second;
    auto agg = timer->calcStat();
    auto tot_nanos = agg.getNanos();
    auto avg_nanos = agg.cycles == 0 ? "N/A" : format_time(agg.getAvgNanos());
    auto avg_cycles = agg.cycles == 0 ? "N/A" : std::to_string(agg.getAvgCycles());
    spdlog::info("{:<{}}{}\t{}\t{}({} cycles)\t{}", timer->name, l, format_time(tot_nanos), agg.cnt,
                 avg_nanos, avg_cycles, timer->desc);
  }
}

template <>
inline void GlobalStat<PerThreadCounter>::printStats() {
  if (stats.size() == 0) {
    spdlog::info("NO COUNTERS");
    return;
  }
  auto l = std::max(8UL, get_max_strlen(stats) + 2);
  spdlog::info("======COUNTERS======");
  spdlog::info("{:<{}}\tCOUNT\tDESCRIPTION", "NAME", l);
  for (const auto &kv : stats) {
    auto counter = kv.second;
    spdlog::info("{:<{}}{}\t{}", counter->name, l, counter->calcStat(), counter->desc);
  }
}

template <>
inline void GlobalStat<NoopTimer>::printStats() {}

template <>
inline void GlobalStat<NoopCounter>::printStats() {}

inline void SimpleStat::printStats() {
  if (stats.size() == 0) {
    spdlog::info("NO USER STATS");
    return;
  }
  auto l = std::max(8UL, get_max_strlen(stats) + 2);
  spdlog::info("======USER STATS======");
  spdlog::info("{:<{}}\tVALUE\tDESCRIPTION", "NAME", l);
  for (const auto &kv : stats) {
    auto val = kv.second->callback();
    spdlog::info("{:<{}}{}\t{}", kv.first, l, val, kv.second->desc);
  }
}

inline void print_timer_stats() { GlobalStat<TimerType>::printStats(); }
inline void print_counter_stats() { GlobalStat<CounterType>::printStats(); }
inline void print_user_stats() { SimpleStat::printStats(); }

inline void print_stats() {
  print_timer_stats();
  print_counter_stats();
  print_user_stats();
}

} // namespace hwstat

#define _TIMER_3(_name, _desc, _prefix)                                                            \
  _prefix hwstat::GlobalStat<hwstat::TimerType> gtimer_##_name(#_name, _desc);                     \
  _prefix thread_local hwstat::TimerType _name(&gtimer_##_name);

#define _COUNTER_3(_name, _desc, _prefix)                                                          \
  _prefix hwstat::GlobalStat<hwstat::CounterType> gcounter_##_name(#_name, _desc);                 \
  _prefix thread_local hwstat::CounterType _name(&gcounter_##_name);

#define DECLARE_TIMER(_name)                                                                       \
  extern hwstat::GlobalStat<hwstat::TimerType> gtimer_##_name;                                     \
  extern thread_local hwstat::TimerType _name;

#define DECLARE_COUNTER(_name)                                                                     \
  extern hwstat::GlobalStat<hwstat::CounterType> gcounter_##_name;                                 \
  extern thread_local hwstat::CounterType _name;

#define _TIMER_2(_name, _desc) _TIMER_3(_name, _desc, static)
#define _TIMER_1(_name) _TIMER_2(_name, "")

#define _COUNTER_2(_name, _desc) _COUNTER_3(_name, _desc, static)
#define _COUNTER_1(_name) _COUNTER_2(_name, "")

#define _STAT_4(_name, _func, _desc, _prefix)                                                      \
  _prefix hwstat::SimpleStat gstat_##_name(#_name, _func, _desc);
#define _STAT_3(_name, _func, _desc) _STAT_4(_name, _func, _desc, static)
#define _STAT_2(_name, _func) _STAT_3(_name, _func, "")
#define _STAT_1(_x) static_assert(false, "Please provide at least two arguments for _STAT macro.");

#define _GET_MACRO_3(_3, _2, _1, _name, ...) _name
#define _GET_MACRO_4(_4, _3, _2, _1, _name, ...) _name

#define TIMER(...) _GET_MACRO_3(__VA_ARGS__, _TIMER_3, _TIMER_2, _TIMER_1)(__VA_ARGS__)
#define COUNTER(...) _GET_MACRO_3(__VA_ARGS__, _COUNTER_3, _COUNTER_2, _COUNTER_1)(__VA_ARGS__)
#define STAT(...) _GET_MACRO_4(__VA_ARGS__, _STAT_4, _STAT_3, _STAT_2, _STAT_1)(__VA_ARGS__)

#endif // _HWSTAT_H
