#pragma once

#include "packet_queue.h"
#include "srsran/adt/span.h"
#include "srsran/ofh/ethernet/ethernet_factories.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/executors/task_executor.h"

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

class MediaTransmitter
{
public:
  MediaTransmitter(srslog::basic_logger&  logger,
                   const std::string&     input_stream,
                   const std::string&     output_stream,
                   srsran::PacketQueue&   packet_queue_,
                   srsran::task_executor& executor,
                   uint16_t               speed_factor);
  void set_eth_builder(srsran::ether::frame_builder* eth_builder_) { eth_builder = eth_builder_; }
  ~MediaTransmitter();
  void start();

  bool               fill_payload(span<uint8_t> payload, size_t& payload_size, bool dummy = false);
  PayloadCheckResult forward_payload(span<const uint8_t> payload);

private:
  srsran::ether::frame_builder* eth_builder;

  srsran::PacketQueue&    packet_queue;
  srsran::task_executor&  executor;
  std::string             input_stream_file_name;
  std::string             output_stream_file_name;
  int                     video_tunnel_in;
  int                     video_tunnel_out;
  srslog::basic_logger&   logger;
  uint16_t                sequence_id = 0;
  std::optional<uint16_t> last_received_sequence_id;

  uint16_t speed_factor;

  bool open_video_tunnel_in();
  bool open_video_tunnel_out();
  void generate_media();
};

bool save_to_binary_file(const void* data_address, std::size_t data_length, const std::string& file_path);