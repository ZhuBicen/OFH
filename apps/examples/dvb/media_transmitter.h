#pragma once

#include "kpi_counter.h"
#include "packet_queue.h"
#include "packet_receiver.h"
#include "srsran/adt/span.h"
#include "srsran/ofh/ethernet/ethernet_factories.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/executors/task_executor.h"

#include <memory>
#include <optional>
#include <stdint.h>
#include <string>

using srsran::span;

enum class PayloadCheckResult {
  OK,
  NO_VIDEO_TUNNEL,
  VIDEO_TUNNEL_BUSY,
  TOO_SMALL_PAYLOAD,
  INVALID_SYNC_HEADER,
  INVALID_SEQUENCE_ID,
  INVALID_LENGTH,
  INVALID_CRC,
};

struct Header {
  uint32_t sync_header;
  uint16_t length;
  uint16_t sequence;
  uint16_t media_length;
} __attribute__((packed));

class MediaTransmitter
{
public:
  MediaTransmitter(srslog::basic_logger&  logger,
                   const std::string&     input_stream,
                   const std::string&     output_stream,
                   srsran::PacketQueue&   packet_queue_,
                   srsran::task_executor& executor,
                   uint16_t               speed_factor,
                   unsigned               initial_num_of_packet,
                   uint16_t               mtu_size,
                   bool                   vairable_mtu,
                   kpi_counter&           tx_video_packet_counter_,
                   kpi_counter&           tx_dummy_packet_counter_);
  void set_eth_builder(srsran::ether::frame_builder* eth_builder_);
  ~MediaTransmitter();
  void start();

  std::optional<Header> fill_media(srsran::Packet packet, bool dummy, uint16_t seq);
  void                  fill_header(srsran::Packet packet, struct Header& header);
  PayloadCheckResult    forward_payload(span<const uint8_t> payload);

private:
  srsran::ether::frame_builder* eth_builder;
  srsran::PacketReceiver        packet_receiver;

  srsran::PacketQueue&    packet_queue;
  srsran::task_executor&  executor;
  std::string             input_stream_file_name;
  std::string             output_stream_file_name;
  int                     video_tunnel_in;
  int                     video_tunnel_out;
  srslog::basic_logger&   logger;
  uint16_t                sequence_id = 0;
  std::optional<uint16_t> last_received_sequence_id;

  uint16_t     speed_factor;
  unsigned     initial_num_of_packet;
  uint16_t     mtu_size;
  bool         variable_mtu;
  kpi_counter& tx_video_packet_counter;
  kpi_counter& tx_dummy_packet_counter;

  bool open_video_tunnel_in();
  bool open_video_tunnel_out();
  void generate_media();
  void calcaute_dummy_packet_crc();
  void create_dummy_ethernet_frame(uint16_t seq);
  void fill_crc(span<uint8_t> payload, bool dummy);
  void fill_dummy_packet_seq(span<uint8_t> payload, uint16_t seq);

  void push_dummy_packet();

  void push_packet_to_send_queue(srsran::Packet packet);

  srsran::Packet       dummy_ethernet_frame;
  uint16_t             ether_head_size = 0;
  static constexpr int CRC_LENGTH      = sizeof(uint16_t);
};

bool save_to_binary_file(const void* data_address, std::size_t data_length, const std::string& file_path);