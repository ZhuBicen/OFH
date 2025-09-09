#pragma once
#include "srsran/ofh/ethernet/ethernet_unique_buffer.h"
#include "srsran/srslog/logger.h"

#include <chrono>
#include <queue>
#include <string>
#include <vector>

namespace srsran {

struct RxPacket {
  int                  sequence_number;
  std::vector<uint8_t> data;

  RxPacket(const RxPacket& other) noexcept : sequence_number(other.sequence_number), data(std::move(other.data)) {}

  RxPacket(int seq, std::vector<uint8_t>&& d) : sequence_number(seq), data(std::move(d)) {}

  bool operator<(const RxPacket& other) const
  {
    return ((sequence_number > other.sequence_number) && (sequence_number - other.sequence_number <= 32768)) ||
           ((sequence_number < other.sequence_number) && (other.sequence_number - sequence_number > 32768));
  }
};

class PacketReceiver
{
private:
  std::priority_queue<RxPacket>              packet_queue;
  uint16_t                                   expected_seq;
  static constexpr int                       MAX_GAP = 32;
  static constexpr std::chrono::milliseconds TIMEOUT{2000};
  std::chrono::steady_clock::time_point      last_packet_time;
  bool                                       is_seq_greater(uint16_t seq1, uint16_t seq2) const;

public:
  int get_buffered_packet_num() { return packet_queue.size(); }
  PacketReceiver(srslog::basic_logger& logger);
  bool                  receive_packet(RxPacket&& packet);
  std::vector<RxPacket> get_sorted_packets();
  bool                  has_pending_packets() const;
  srslog::basic_logger& logger;
};
} // namespace srsran