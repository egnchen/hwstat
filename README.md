# HWStat

Header-only library for low-overhead performance counting on X86 platform.

## Getting started

You'll need a C++17 compiler and `spdlog`.

* Install `spdlog`.
  * Ubuntu/Debian: `sudo apt install libspdlog-dev`
  * Or follow instructions [here](https://github.com/gabime/spdlog?tab=readme-ov-file#package-managers).
* Copy `hwstat.h` to your own project.
* Include the header in your cpp files.

## Usage

```cpp
// define a counter
COUNTER(testCounter)
COUNTER(anotherCounter, "description for the counter")
COUNTER(staticCounter, "this will add 'static' before the counter variable definition", static)

testCounter++; // add to a counter
testCounter += 2; // add twice

// define a timer the same way as counter
TIMER(testTimer)
TIMER(testTimer, "description for the timer")
TIMER(testTimer, "static timer", static)

// use the Stopwatch API to record time
using hwstat::Stopwatch;
Stopwatch sw(testTimer); // construct & start the timer
// ... do something time consuming ...
sw.stop(); // stop & count as 1

sw.restart(); // restart the timer
// ... do something time consuming ... 

// you can do this multiple times
sw.pause(); // pause the timer
// ... do something time consuming but irrelevant ...
sw.resume(); // resume the timer

// ... do something time consuming ...
sw.stop(); // stop & count as 1


// use the ScopedTimer API to record time
// ScopedTimer utilizes RAII to record time, like std::lock_guard
using hwstat::ScopedTimer;
{
  ScopedTimer(testTimer); // begin timing
  // ... do something time consuming ...

  // at the end of the scope the timer would stop & count as 1
}

// print statistics
hwstat::print_stats(); // print all statistics
// or print them separately
hwstat::print_timer_stats();
hwstat::print_counter_stats();
```

## Implementation details

Counter & timer values are stored in `thread_local` variables so performance is scalable. You may see noticeable performance degration if your library is dynamically linked(depending on how thread local storage is implemented by your compiler).

Performance overhead is modest since `rdtsc` instruction is used to record time. Typical latency is 20~30 cycles on x86 platform(single-digit ns, ~50% lower than `clock_gettime`).
