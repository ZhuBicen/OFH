#include "packet_sender.h"
#include "media_transmitter.h"

#include <chrono>
#include <future>
#include <thread>

#ifdef DPDK_FOUND
#include "srsran/hal/dpdk/dpdk_eal_factory.h"
#endif

constexpr unsigned MAX_BURST_SIZE = 64;

using namespace srsran;

PacketSender::PacketSender(srslog::basic_logger&   logger_,
                           task_executor&          executor_,
                           dvb_tx_sim_transceiver& transceiver_,
                           PacketQueue&            queue_,
                           kpi_counter&            tx_bytes_,
                           unsigned                packet_delay_in_nano_seconds_) :
  logger(logger_),
  executor(executor_),
  packets(queue_),
  transceiver(transceiver_),
  tx_bytes(tx_bytes_),
  packet_delay_in_nano_seconds(packet_delay_in_nano_seconds_)
{
}

void PacketSender::start()
{
  std::promise<void> p;
  std::future<void>  fut = p.get_future();

  if (!executor.defer([this, &p]() {
        p.set_value();
        send_loop();
      })) {
    report_fatal_error("Failed to defer packet processing task");
  }
  fut.wait();
}
bool save_to_binary_file(const void* data_address, std::size_t data_length, const std::string& file_path);

void PacketSender::send_loop()
{
  static bool save_first_send_frame = true;
  while (true) {
    static_vector<span<const uint8_t>, 1> frame_burst;
    std::vector<Packet>                   cache_packets;
    for (size_t i = 0; i < packets.size(); i++) {
      Packet packet;
      packets.try_pop(packet);
      tx_bytes.increment(packet->size());
      cache_packets.push_back(packet);
      if (save_first_send_frame) {
        save_to_binary_file(packet->data(), packet->size(), "packet_sender_first_packet.bin");
        save_first_send_frame = false;
      }
      frame_burst.emplace_back(packet->data(), packet->size());
      auto seq       = get_packet_seq(packet->data(), packet->size());
      auto send_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
      // logger.info(
      //     "Sending packet with seq={} size={} at time {}", seq, packet->size(),
      //     send_time.time_since_epoch().count());
      send_nano_seconds[seq] = send_time;
      transceiver.send(frame_burst);
      frame_burst.clear();

      std::this_thread::sleep_for(std::chrono::nanoseconds(packet_delay_in_nano_seconds));
    }
  }
}