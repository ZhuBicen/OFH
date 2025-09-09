#pragma once

#include "dvb_tx_sim_transceiver.h"
#include "kpi_counter.h"
#include "packet_queue.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/executors/task_executor.h"

#include <atomic>
#include <cstdint>

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

public:
  PacketSender(srslog::basic_logger&   logger_,
               task_executor&          executor_,
               dvb_tx_sim_transceiver& transceiver_,
               PacketQueue&            queue_,
               kpi_counter&            tx_bytes_,
               unsigned                packet_delay_in_nano_seconds_);
  void start();
};
} // namespace srsran