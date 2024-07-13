#ifndef _HWSTAT_H
#define _HWSTAT_H

#include <cassert>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <x86intrin.h>

#include <spdlog/spdlog.h>

// use this to disable all stats

// #define NO_STAT

// predefine the frequency of _rdtsc instruction
// if not defined, the frequency will be measured automatically everytime the program starts(takes
// about 10ms)

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
  GlobalStat(const GlobalStat<T> &) = delete;
  void reg(T *instance) {
    std::lock_guard<std::mutex> guard(mtx);
    instances.insert(instance);
  }
  void dereg(T *instance) {
    std::lock_guard<std::mutex> guard(mtx);
    agg = instance->aggregate(agg);
    instances.erase(instance);
  }
  typename T::AggregateType calcAggregate() {
    auto nagg = agg;
    std::lock_guard<std::mutex> guard(mtx);
    for (auto i : instances) {
      nagg = i->aggregate(nagg);
    }
    return nagg;
  }
  static void printStats();

private:
  static inline std::mutex gMtx;
  static inline std::map<const char *, GlobalStat<T> *> stats;
};

struct PerThreadTimer {
  using GlobalTimer = GlobalStat<PerThreadTimer>;
  using AggregateType = std::pair<unsigned long, unsigned long>;
  unsigned long cycle = 0;
  unsigned long cnt = 0;
  GlobalTimer *global_timer;
  PerThreadTimer(GlobalTimer *globalTimer) : global_timer(globalTimer) { globalTimer->reg(this); }
  ~PerThreadTimer() { global_timer->dereg(this); }
  void add(unsigned long dc = 0) {
    cycle += dc;
    cnt++;
  }
  AggregateType aggregate(AggregateType prev) {
    prev.first += cycle;
    prev.second += cnt;
    return prev;
  }
};

struct NoopTimer {
  using GlobalTimer = GlobalStat<NoopTimer>;
  using AggregateType = std::pair<unsigned long, unsigned long>;
  NoopTimer(GlobalTimer *timer) {}
  ~NoopTimer() {}
  void add(unsigned long dc = 0) {}
  AggregateType aggregate(AggregateType prev) { return AggregateType{}; }
};

struct PerThreadCounter {
  using GlobalCounter = GlobalStat<PerThreadCounter>;
  using AggregateType = unsigned long;
  unsigned long cnt = 0;
  GlobalCounter *global_counter;
  PerThreadCounter(GlobalCounter *globalCounter) : global_counter(globalCounter) {
    globalCounter->reg(this);
  }
  ~PerThreadCounter() { global_counter->dereg(this); }
  void add(int d = 1) { cnt += d; }
  unsigned long operator++() { return ++cnt; }
  unsigned long operator++(int) { return cnt++; }
  unsigned long operator+=(unsigned long d) { return cnt += d; }
  AggregateType aggregate(AggregateType prev) { return prev + cnt; }
};

struct NoopCounter {
  using GlobalCounter = GlobalStat<NoopCounter>;
  using AggregateType = unsigned long;
  NoopCounter(GlobalCounter *globalCounter) {}
  ~NoopCounter() {}
  void add(int d = 1) {}
  unsigned long operator++() { return 0; }
  unsigned long operator++(int) { return 0; }
  unsigned long operator+=(unsigned long d) { return 0; }
  AggregateType aggregate(AggregateType prev) { return AggregateType{}; }
};

#ifndef NO_STAT
using CounterType = PerThreadCounter;
using TimerType = PerThreadTimer;
#else
using CounterType = NoopCounter;
using TimerType = NoopTimer;
#endif

#ifndef NO_STAT

class Stopwatch {
  TimerType &timer;
  unsigned long st;
  unsigned long agg = 0;

public:
  Stopwatch(TimerType &timer) : timer(timer) { restart(); }
  void pause() { agg += _rdtsc() - st; }
  void resume() { st = _rdtsc(); }
  void restart() { resume(); }
  void stop() {
    pause();
    timer.add(agg);
    agg = 0;
  }
};

#else

class Stopwatch {
public:
  Stopwatch(TimerType &timer) {}
  void pause() {}
  void resume() {}
  void restart() {}
  void stop() {}
};

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

static inline double measureTscFreqGhz(int sleep_ms = 10) {
#ifdef TSC_FREQ_GHZ
  return TSC_FREQ_GHZ;
#else
  auto start_clk = std::chrono::high_resolution_clock::now();
  unsigned long start = _rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  auto end_clk = std::chrono::high_resolution_clock::now();
  unsigned long end = _rdtsc();
  auto count_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_clk - start_clk).count();
  return double(end - start) / count_ns;
#endif
}

static inline const double kFreqGhz = measureTscFreqGhz();

template <>
inline void GlobalStat<PerThreadTimer>::printStats() {
  spdlog::info("======TIMERS(freq = {:.3}Ghz)======", kFreqGhz);
  spdlog::info("{:<16}TIME\tCOUNT\tAVERAGE\t\tDESCRIPTION", "NAME");
  for (const auto kv : stats) {
    auto timer = kv.second;
    auto agg = timer->calcAggregate();
    auto tot_nanos = agg.first / kFreqGhz;
    auto avg_nanos = agg.second == 0 ? "N/A" : format_time(tot_nanos / agg.second);
    auto avg_cycles = agg.second == 0 ? "N/A" : std::to_string(agg.first / agg.second);
    spdlog::info("{:<16}{}\t{}\t{}({} cycles)\t{}", timer->name, format_time(tot_nanos), agg.second,
                 avg_nanos, avg_cycles, timer->desc);
  }
}

template <>
inline void GlobalStat<PerThreadCounter>::printStats() {
  spdlog::info("======COUNTERS======");
  spdlog::info("{:<16}\tCOUNT\tDESCRIPTION", "NAME");
  for (const auto &kv : stats) {
    auto counter = kv.second;
    spdlog::info("{:<16}{}\t{}", counter->name, counter->calcAggregate(), counter->desc);
  }
}

template <>
inline void GlobalStat<NoopTimer>::printStats() {}

template <>
inline void GlobalStat<NoopCounter>::printStats() {}

inline void print_timer_stats() { GlobalStat<TimerType>::printStats(); }
inline void print_counter_stats() { GlobalStat<CounterType>::printStats(); }

inline void print_stats() {
  print_timer_stats();
  print_counter_stats();
}

} // namespace hwstat

#define _TIMER_3(name, desc, prefix)                                                               \
  prefix hwstat::GlobalStat<hwstat::TimerType> gtimer_##name(#name, desc);                         \
  prefix thread_local hwstat::TimerType name(&gtimer_##name);

#define _COUNTER_3(name, desc, prefix)                                                             \
  prefix hwstat::GlobalStat<hwstat::CounterType> gcounter_##name(#name, desc);                     \
  prefix thread_local hwstat::CounterType name(&gcounter_##name);

#define DECLARE_TIMER(name)                                                                        \
  extern hwstat::GlobalStat<hwstat::TimerType> gtimer_##name;                                      \
  extern thread_local hwstat::TimerType name;

#define DECLARE_COUNTER(name)                                                                      \
  extern hwstat::GlobalStat<hwstat::CounterType> gcounter_##name;                                  \
  extern thread_local hwstat::CounterType name;

#define _TIMER_2(name, desc) _TIMER_3(name, desc, )
#define _TIMER_1(name) _TIMER_2(name, "")

#define _COUNTER_2(name, desc) _COUNTER_3(name, desc, )
#define _COUNTER_1(name) _COUNTER_2(name, "")

#define _GET_MACRO(_3, _2, _1, NAME, ...) NAME

#define TIMER(...) _GET_MACRO(__VA_ARGS__, _TIMER_3, _TIMER_2, _TIMER_1)(__VA_ARGS__)
#define COUNTER(...) _GET_MACRO(__VA_ARGS__, _COUNTER_3, _COUNTER_2, _COUNTER_1)(__VA_ARGS__)

#endif
