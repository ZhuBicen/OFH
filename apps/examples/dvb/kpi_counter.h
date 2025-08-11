#pragma once
#include <atomic>
#include <cstdint>
class kpi_counter
{
  std::atomic<uint64_t> counter{0};
  uint64_t              last_value_printed = 0U;

public:
  uint64_t get_value()
  {
    uint64_t current_value = counter.load(std::memory_order_relaxed);
    uint64_t total         = current_value - last_value_printed;
    last_value_printed     = current_value;
    return total;
  }

  void increment(unsigned n = 1) { counter.fetch_add(n, std::memory_order_relaxed); }
};