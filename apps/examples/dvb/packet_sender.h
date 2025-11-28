#pragma once

#include "dvb_tx_sim_transceiver.h"
#include "kpi_counter.h"
#include "packet_queue.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/executors/task_executor.h"

#include <atomic>
#include <cstdint>

using MicroSecond = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

namespace srsran {
class PacketSender
{
  srslog::basic_logger&   logger;
  task_executor&          executor;
  PacketQueue&            packets;
  dvb_tx_sim_transceiver& transceiver;
  kpi_counter&            tx_bytes;
  unsigned                packet_delay_in_nano_seconds;

  void send_loop();

  std::array<MicroSecond, std::numeric_limits<uint16_t>::max()> send_nano_seconds;

public:
  PacketSender(srslog::basic_logger&   logger_,
               task_executor&          executor_,
               dvb_tx_sim_transceiver& transceiver_,
               PacketQueue&            queue_,
               kpi_counter&            tx_bytes_,
               unsigned                packet_delay_in_nano_seconds_);
  void        start();
  MicroSecond get_send_time(uint16_t seq) { return send_nano_seconds[seq]; }
};
} // namespace srsran