#include "packet_receiver.h"
#include <iostream>

using namespace srsran;

PacketReceiver::PacketReceiver() : expected_seq(0)
{
  last_packet_time = std::chrono::steady_clock::now();
}

bool PacketReceiver::is_seq_greater(uint16_t seq1, uint16_t seq2) const
{
  return ((seq1 > seq2) && (seq1 - seq2 <= 32768)) || ((seq1 < seq2) && (seq2 - seq1 > 32768));
}

void PacketReceiver::receive_packet(RxPacket&& rxPacket)
{
  // if (!last_received_sequence_id) {
  //   last_received_sequence_id = seq_id;
  // } else {
  //   uint16_t expected_sequence = (last_received_sequence_id.value() + 1) % UINT16_MAX;
  //   if (header.sequence != expected_sequence) {
  //     logger.warning("Received sequence ID {} does not match expected {}",
  //                    ntohs(header.sequence),
  //                    last_received_sequence_id.value());
  //     last_received_sequence_id = (uint16_t)header.sequence;
  //     return PayloadCheckResult::INVALID_SEQUENCE_ID;
  //   }
  //   last_received_sequence_id = seq_id;
  // }

  if (!is_seq_greater(expected_seq, rxPacket.sequence_number)) {
    packet_queue.push(rxPacket);
    last_packet_time = std::chrono::steady_clock::now();
    // std::cout << "Received packet: seq=" << seq << ", data=" << data << "\n";
  } else {
    std::cout << "Discarding old packet: seq=" << rxPacket.sequence_number << ", expected=" << expected_seq << "\n";
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
    } else if (timeout || is_seq_greater(top.sequence_number, expected_seq + MAX_GAP)) {
      std::cout << "Detected packet loss: seq=" << expected_seq << "\n";
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