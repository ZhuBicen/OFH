#include "packet_receiver.h"
#include <iostream>

using namespace srsran;

PacketReceiver::PacketReceiver(srslog::basic_logger& logger_) : expected_seq(0), logger(logger_)
{
  last_packet_time = std::chrono::steady_clock::now();
}

bool PacketReceiver::is_seq_greater(uint16_t seq1, uint16_t seq2) const
{
  return ((seq1 > seq2) && (seq1 - seq2 <= 32768)) || ((seq1 < seq2) && (seq2 - seq1 > 32768));
}

bool PacketReceiver::receive_packet(RxPacket&& rxPacket)
{
  if (expected_seq == rxPacket.sequence_number) {
    packet_queue.push(rxPacket);
    last_packet_time = std::chrono::steady_clock::now();
    return true;
  } else if (is_seq_greater(rxPacket.sequence_number, expected_seq)) {
    // logger.warning(
    //     "Out of order seq num, cache it, expected: {}, received: {}", expected_seq, rxPacket.sequence_number);
    packet_queue.push(rxPacket);
    last_packet_time = std::chrono::steady_clock::now();
    return true;
  } else {
    logger.error("Discarding old packet: seq= {}, , expected= {}, buffered={}, top={}",
                 rxPacket.sequence_number,
                 expected_seq,
                 packet_queue.size(),
                 packet_queue.top().sequence_number);
    last_packet_time = std::chrono::steady_clock::now();
    return false;
  }
}

std::vector<RxPacket> PacketReceiver::get_sorted_packets()
{
  std::vector<RxPacket> sorted_packets;

  auto now     = std::chrono::steady_clock::now();
  bool timeout = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_packet_time) > TIMEOUT;

  while (!packet_queue.empty()) {
    RxPacket top = std::move(packet_queue.top());
    if (top.sequence_number == expected_seq) {
      sorted_packets.push_back(std::move(top));
      packet_queue.pop();
      expected_seq = (expected_seq + 1) % UINT16_MAX;
    } else if (timeout || packet_queue.size() >= MAX_GAP) {
      logger.error(
          "Detected packet loss: seq [{}, {}), buffered={}", expected_seq, top.sequence_number, packet_queue.size());
      expected_seq = top.sequence_number;
      sorted_packets.push_back(std::move(top));
      packet_queue.pop();
      expected_seq = (expected_seq + 1) % UINT16_MAX;
    } else {
      break;
    }
  }
  return sorted_packets;
}

bool PacketReceiver::has_pending_packets() const
{
  return !packet_queue.empty();
}