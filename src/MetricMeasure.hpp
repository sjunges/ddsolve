#ifndef METRICMEASURE_HPP
#define METRICMEASURE_HPP

/**
 * @file MetricMeasure.hpp
 * @brief Thread-local monotonic time measurement utility.
 *
 * MetricMeasure provides a lightweight, thread-safe mechanism for recording
 * elapsed time intervals using POSIX monotonic clocks. Each thread obtains
 * its own slot in a fixed-size array of vectors, and intervals are stored
 * as timespec deltas between @p start() and @p stop() calls.
 *
 * @tparam kMaxThreads  Maximum number of concurrent threads supported.
 */

#include <array>
#include <atomic>
#include <ctime>
#include <fstream>
#include <ios>
#include <memory>
#include <print>
#include <ranges>
#include <type_traits>
#include <vector>

namespace ddsolve {

/**
 * @brief Thread-local monotonic time measurement.
 *
 * Records time intervals between @p start() and @p stop() calls on a
 * per-thread basis using CLOCK_MONOTONIC. Results can be printed to
 * stdout or written to a file.
 *
 * @tparam kMaxThreads  Maximum number of concurrent threads supported.
 */
template <size_t kMaxThreads>
class MetricMeasure {
 public:
  /**
   * @brief Get or create a thread-local index for this measurement session.
   *
   * Assigns a unique slot from a global counter to the calling thread using
   * thread-local storage. If the slot has not yet been allocated, a new
   * vector is created to store timespec entries.
   *
   * @return A thread-local index used to identify this thread's data.
   */
  constexpr uint32_t getLocalIndex() {
    thread_local uint32_t local_index{
        next_idx.fetch_add(1, std::memory_order_relaxed)};

    if (!data_.at(local_index)) {
      data_.at(local_index) = std::make_unique<std::vector<timespec>>();
    }

    return local_index;
  }

  /**
   * @brief Record the current monotonic time as a start point.
   *
   * Calls @p clock_gettime(CLOCK_MONOTONIC) and appends the result to
   * the calling thread's vector.
   */
  void start() {
    auto& vec{*data_[getLocalIndex()]};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    vec.push_back(t);
  }

  /**
   * @brief Record the current monotonic time as an end point.
   *
   * Computes the elapsed duration since the last @p start() call and stores
   * the delta (as a timespec) in the calling thread's vector.
   */
  void stop() {
    auto& last{data_[getLocalIndex()]->back()};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    t.tv_sec = t.tv_sec - last.tv_sec;
    t.tv_nsec = t.tv_nsec - last.tv_nsec;
    if (t.tv_nsec < 0) {
      t.tv_sec -= 1;
      constexpr auto one_sec{1'000'000'000};
      t.tv_nsec += one_sec;
    }

    last = t;
  }

  /**
   * @brief Print all recorded intervals to stdout.
   *
   * Outputs each thread's intervals as space-separated values.
   * Format: `<thread_id> <nanoseconds>` or `<thread_id>
   * <seconds><nanoseconds>`.
   *
   * @note This method is not thread-safe and should only be called from
   *       a single thread after all measurement threads have finished.
   */
  void print() {
    // Not threadsafe!

    for (std::size_t tid = 0; tid < data_.size(); ++tid) {
      auto&& i = data_[tid];
      if (i) {
        for (auto&& t : *i) {
          if (t.tv_sec == 0) {
            std::println("{} {}", tid, t.tv_nsec);
          } else {
            std::println("{} {}{:09}", tid, t.tv_sec, t.tv_nsec);
          }
        }
      }
    }
  }

  /**
   * @brief Write all recorded intervals to a file.
   *
   * Writes to `/tmp/data.txt` in the same format as @p print().
   * The file is truncated before writing.
   *
   * @note This method is not thread-safe and should only be called from
   *       a single thread after all measurement threads have finished.
   */
  void write() {
    // Not threadsafe!
    auto file{std::ofstream{"/tmp/data.txt",
                            std::ios_base::out | std::ios_base::trunc}};

    for (std::size_t tid = 0; tid < data_.size(); ++tid) {
      auto&& i = data_[tid];
      if (i) {
        for (auto&& t : *i) {
          if (t.tv_sec == 0) {
            std::println(file, "{} {}", tid, t.tv_nsec);
          } else {
            std::println(file, "{} {}{:09}", tid, t.tv_sec, t.tv_nsec);
          }
        }
      }
    }
  }

 private:
  /**
   * @brief Atomic counter for assigning thread-local indices.
   */
  std::atomic_uint32_t next_idx{0};

  /**
   * @brief Per-thread storage for measurement intervals.
   *
   * Each element is either null (unused) or a vector of timespec deltas
   * representing recorded intervals for that thread.
   */
  std::array<std::unique_ptr<std::vector<timespec>>, kMaxThreads> data_;
};

static_assert(std::is_signed_v<decltype(timespec::tv_nsec)>);

}  // namespace ddsolve

#endif /* METRICMEASURE_HPP */
