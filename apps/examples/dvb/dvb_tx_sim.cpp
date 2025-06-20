/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "dvb_tx_sim_appconfig.h"
#include "dvb_tx_sim_cli11_schema.h"
#include "dvb_tx_sim_timing_notifier.h"
#include "dvb_tx_sim_transceiver.h"
#include "helpers.h"
#include "srsran/adt/circular_map.h"
#include "srsran/adt/expected.h"
#include "srsran/ofh/compression/compression_params.h"
#include "srsran/ofh/ecpri/ecpri_constants.h"
#include "srsran/ofh/ecpri/ecpri_packet_properties.h"
#include "srsran/ofh/ethernet/ethernet_factories.h"
#include "srsran/ofh/ofh_constants.h"
#include "srsran/ofh/serdes/ofh_message_properties.h"
#include "srsran/ran/resource_block.h"
#include "srsran/ran/slot_point.h"
#include "srsran/srslog/logger.h"
#include "srsran/support/config_parsers.h"
#include "srsran/support/executors/task_execution_manager.h"
#include "srsran/support/executors/task_executor.h"
#include "srsran/support/format_utils.h"
#include "srsran/support/signal_handling.h"
#include "fmt/chrono.h"
#include <arpa/inet.h>
#include <errno.h> // For errno
#include <fcntl.h> // For open, O_RDONLY, O_NONBLOCK
#include <random>
#include <srsran/adt/to_array.h>
#include <sys/stat.h>  // For mkfifo
#include <sys/types.h> // For open, mkfifo
#include <unistd.h>    // For read, close, unlink

#ifdef DPDK_FOUND
#include "srsran/hal/dpdk/dpdk_eal_factory.h"
#endif

using namespace srsran;
using namespace ofh;
using namespace ether;

/// Ethernet packet size.
static constexpr unsigned ETHERNET_FRAME_SIZE = 2048;

/// Maximum number of symbols in a slot, considering normal cyclic prefix.
static constexpr size_t MAX_SEND_SYMBOLS = 15;

/// Depending on configured compression parameters one UL U-Plane message may occupy up to 2 Ethernet packets.
static constexpr size_t MAX_NOF_PACKETS_PER_UPLANE_MESSAGE = 100;

static constexpr unsigned MAX_SAVE_FRAME = 8;

static constexpr unsigned MAX_DVB_FRAME_SIZE = 451584;

namespace {

/// RU emulator configuration structure.
struct dvb_tx_sim_config {
  /// RU emulator Ethernet MAC address.
  mac_address src_mac;
  /// DU Ethernet MAC address.
  mac_address dst_mac;
  /// VLAN tag.
  unsigned vlan_tag;

  unsigned nof_prb;

  unsigned mtu;

  std::string input_file;
};

/// Helper structure used to group OFH header parameters.
struct header_parameters {
  uint8_t  frame_id;
  uint16_t payload_size;
  uint32_t start_prb;
  uint16_t nof_prbs;
  bool     last_pkg;
};

typedef struct dvb_transport_header_t {
  uint8_t reserved      : 2;
  uint8_t last_pkg_flag : 1;
  uint8_t ef            : 1;
  uint8_t version       : 4;
  uint8_t dvb_master_id;
  uint16_t payload;
  uint16_t seqid;
  uint8_t frame_id;
  uint16_t block_number;
  uint32_t start_block;
} __attribute__((__packed__)) dvb_transport_header_t;

typedef struct dvb_transport_extend_header_t {
  uint8_t reserved      : 2;
  uint8_t last_pkg_flag : 1;
  uint8_t ef            : 1;
  uint8_t version       : 4;
  uint8_t dvb_master_id;
  uint16_t payload;
  uint16_t seqid;
  uint8_t frame_id;
  uint16_t block_number;
  uint32_t start_block;
  uint8_t modcod;
  uint32_t scale_factor;
} __attribute__((__packed__)) dvb_transport_extend_header_t;

/// One symbol may require up to two byte buffers depending on configured compression parameters.
using symbol_buffer = static_vector<std::vector<uint8_t>, MAX_NOF_PACKETS_PER_UPLANE_MESSAGE>;

/// Array of symbol buffers, representing symbols of one eAxC.
using eaxc_buffers = static_vector<symbol_buffer, MAX_SEND_SYMBOLS>;

/// Aggregates information received in a message from DU.
struct rx_message_info {
  unsigned          frame_id;
  unsigned          start_prb;
  unsigned          number_of_prbs;
  bool              end_of_frame;
  unsigned          offset;
  unsigned          seq_id;
};

/// \brief OFH packet decoding failure codes.
/// drop    - packet must be dropped (it is not an eCPRI OFH packet).
/// corrupt - packet contains OFH message with valid seqID, but contains either an undefined in the ORAN specification
///           value, unsupported value (e.g. compression parameters) or unconfigured value (e.g. eAxC value).
enum class decoder_error_codes { drop, corrupt };

} // namespace

namespace {

/// Analyzes content of received OFH packets.
/// Returns decoded message parameters on success, otherwise an error code (see \c decoder_error_codes).
static expected<rx_message_info, decoder_error_codes>
decode_rx_message(span<const uint8_t> packet, srslog::basic_logger& logger)
{
  rx_message_info message_info;
  struct dvb_transport_header_t *head;
  std::size_t offset = 0;
  // Drop non OFH packet.
  if (packet.size() < 26) {
    logger.debug("Dropping packet of size smaller than 26 bytes");
    return make_unexpected(decoder_error_codes::drop);
  }

  // Verify the Ethernet type is eCPRI.
  uint16_t eth_type = (uint16_t(packet[12]) << 8u) | packet[13];
  if (eth_type == 0x8100) {
    eth_type = (uint16_t(packet[16]) << 8u) | packet[17];
    head = (struct dvb_transport_header_t *)(packet.data() + 18);
    offset = 18;
  } else {
    head = (struct dvb_transport_header_t *)(packet.data() + 14);
    offset = 14;
  }

  if (eth_type != ECPRI_ETH_TYPE) {
    logger.debug("Dropping packet as it is not of eCPRI type");
    return make_unexpected(decoder_error_codes::drop);
  }
  if (head->ef) {
    offset += sizeof(dvb_transport_extend_header_t);
  } else {
    offset += sizeof(dvb_transport_header_t);
  }
  message_info.frame_id = head->frame_id;
  message_info.start_prb = ntohl(head->start_block);
  message_info.number_of_prbs = ntohs(head->block_number);
  message_info.end_of_frame = head->last_pkg_flag;
  message_info.offset = offset;
  message_info.seq_id = ntohs(head->seqid);

  return message_info;
}

class dvb_frame_writer {
  std::ofstream output_file;
  srslog::basic_logger&    logger;
  unsigned frame_count;
  unsigned frame_id;
  unsigned start_prb;
  unsigned number_of_prbs;
  unsigned current_frame_offset;

  public:
    dvb_frame_writer(std::string file_name, srslog::basic_logger& logger_)
      : output_file(file_name, std::ios::binary), logger(logger_)
    {
      frame_count = 0;
      frame_id = 0;
      start_prb = 0;
      number_of_prbs = 0;
      current_frame_offset = 0;
    };

    int write_frame(rx_message_info message_info, const span<const uint8_t>& frame)
    {
      if (frame_count >= MAX_SAVE_FRAME) {
        logger.info("dvb frame (size = {}) have been saved done.", current_frame_offset);
        return -1;
      }
      if (message_info.start_prb != (start_prb + number_of_prbs)) {
        logger.error("dvb frame maybe lost some data block({} - {})", start_prb + number_of_prbs, message_info.start_prb);
      }

      start_prb = message_info.start_prb;
      number_of_prbs = message_info.number_of_prbs;
      if (message_info.end_of_frame) {
        frame_count++;
        start_prb = 0;
        number_of_prbs = 0;
      }
      output_file.write((const char*)frame.data(), frame.size());
      current_frame_offset += frame.size();
      return frame.size();
    }
};

static bool change_fifo_buffer_size(int fd)
{
  long current_size;
  // 3. Get the current pipe buffer size (optional, for verification)
  int ret = fcntl(fd, F_GETPIPE_SZ);
  if (ret == -1) {
    perror("fcntl F_GETPIPE_SZ");
    // Don't exit, as setting might still work even if getting fails (less common)
    fprintf(stderr, "Could not get current pipe size. Error: %s\n", strerror(errno));
    current_size = -1; // Indicate failure
  } else {
    current_size = (long)ret;
    fprintf(stderr, "Current pipe buffer size: %ld bytes\n", current_size);
  }
#define DESIRED_PIPE_SIZE (40 * 1024 * 1024) // 4 MB
  // 4. Set the new pipe buffer size
  fprintf(stderr, "Attempting to set pipe buffer size to %d bytes...\n", DESIRED_PIPE_SIZE);
  ret = fcntl(fd, F_SETPIPE_SZ, DESIRED_PIPE_SIZE);
  if (ret == -1) {
    perror("fcntl F_SETPIPE_SZ");
    fprintf(stderr, "Failed to set pipe size. Error: %s\n", strerror(errno));
    fprintf(stderr, "Possible reasons:\n");
    fprintf(stderr,
            "  - Desired size exceeds /proc/sys/fs/pipe-max-size (%ld bytes on my system).\n",
            current_size); // current_size might be wrong if F_GETPIPE_SZ failed
    fprintf(stderr, "  - Insufficient privileges (need CAP_SYS_RESOURCE if exceeding limits).\n");
  } else {
    fprintf(stderr, "fcntl F_SETPIPE_SZ returned %d. (This is often the actual size set by kernel)\n", ret);
    fprintf(stderr, "New pipe buffer size set successfully to approximately %d bytes.\n", ret);
  }

  // 5. Verify the new size (optional)
  ret = fcntl(fd, F_GETPIPE_SZ);
  if (ret == -1) {
    perror("fcntl F_GETPIPE_SZ (after set)");
    fprintf(stderr, "Could not verify new pipe size. Error: %s\n", strerror(errno));
    return false;
  } else {
    fprintf(stderr, "Verified actual new pipe buffer size: %ld bytes\n", (long)ret);
    return true;
  }
}
/// RU emulator receives OFH traffic and replies with UL packets to a DU.
class dvb_tx_sim : public frame_notifier, public dvb_symbol_boundary_notifier
{
  /// Helper class that represents a KPI counter.
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

  srslog::basic_logger&    logger;
  task_executor&           tx_executor;
  task_executor&           prepare_executor;
  task_executor&           save_executor;
  dvb_tx_sim_transceiver& transceiver;

  // Timing window checkers, store statistics of early/late/on-time packets.
  // RU emulator configuration.
  const dvb_tx_sim_config cfg;
  // Pre-generated test data for each symbol for each configured eAxC.
  std::array<eaxc_buffers, 2> test_frame_data;
  // Keeps track of last used seq_id for each eAxC.
  circular_map<unsigned, uint16_t, MAX_SUPPORTED_EAXC_ID_VALUE> seq_counters;
  // Stores the list of configured eAxC.
  static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> ul_eaxc;

  // Other KPI counters.
  kpi_counter rx_total_counter;
  kpi_counter tx_total_counter;
  kpi_counter corrupt_counter;
  kpi_counter dropped_counter;
  std::unique_ptr<ether::frame_builder>     eth_builder;

  int video_tunnel_in;
  int video_tunnel_out;
  bool need_save_frame;
  bool start_save_frame;
  std::unique_ptr<dvb_frame_writer> frame_writer;

public:
  dvb_tx_sim(srslog::basic_logger&    logger_,
              task_executor&          tx_executor_,
              task_executor&          prepare_executor_,
              task_executor&          save_executor_,
              dvb_tx_sim_transceiver& transceiver_,
              dvb_tx_sim_config       cfg_) :
    logger(logger_),
    tx_executor(tx_executor_),
    prepare_executor(prepare_executor_),
    save_executor(save_executor_),
    transceiver(transceiver_),
    cfg(cfg_),
    video_tunnel_in(0),
    video_tunnel_out(0),
    need_save_frame(false),
    start_save_frame(false)
  {
    seq_counters.insert(0, 0);
    ether::vlan_frame_params ether_params;
    ether_params.eth_type        = ether::ECPRI_ETH_TYPE;
    ether_params.tci             = cfg_.vlan_tag;
    ether_params.mac_dst_address = cfg_.dst_mac;
    ether_params.mac_src_address = cfg_.src_mac;

    if (cfg.vlan_tag) {
      eth_builder = ether::create_vlan_frame_builder(ether_params);
    } else {
      eth_builder = ether::create_frame_builder(ether_params);
    }
    const char* video_tunnel_in_fifo_name = "/tmp/video_tunnel_in";

    if (mkfifo(video_tunnel_in_fifo_name, 0666) == -1 && errno != EEXIST) {
      logger.error("failed to create video fifo");
    }
    video_tunnel_in = open(video_tunnel_in_fifo_name, O_RDONLY | O_NONBLOCK);
    if (video_tunnel_in == -1) {
      logger.error("Failed to open video tunnel out");      
    } else {
      logger.info("open video tunnel in successful");
    }

    const char* video_tunnel_out_fifo_name = "/tmp/video_tunnel_out";
    if (mkfifo(video_tunnel_out_fifo_name, 0666) == -1 && errno != EEXIST) {
      logger.error("failed to create video out fifo. Error: {}", strerror(errno));
      return;
    }
    while (true) {
      video_tunnel_out = open(video_tunnel_out_fifo_name, O_WRONLY | O_NONBLOCK);
    if (video_tunnel_out == -1) {
        logger.warning("Retrying to open video tunnel out. Error: {}", strerror(errno));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      logger.info("open video tunnel out successful");
      change_fifo_buffer_size(video_tunnel_out);
      break;
    }     
  }

  // See interface for documentation.
  void on_new_frame(unique_rx_buffer buffer) override
  {
    span<const uint8_t> payload = buffer.data();
    auto decoded_message_info = decode_rx_message(payload, logger);
    if (!decoded_message_info.has_value()) {
      switch (decoded_message_info.error()) {
        case decoder_error_codes::corrupt:
          corrupt_counter.increment();
          break;
        case decoder_error_codes::drop:
          dropped_counter.increment();
          break;
      }
      return;
    }
    rx_total_counter.increment();
    auto message_info = decoded_message_info.value();

    span<const uint8_t> frame1 = buffer.data().subspan(message_info.offset, buffer.data().size() - message_info.offset);
    const unsigned char* header1 = frame1.data();
    if (header1[0] == 'A' && header1[1] == 'B' && header1[2] == 'C' && header1[3] == 'D') {
      if (!save_executor.defer([this, message_info, b = std::move(buffer)] {
            span<const uint8_t>  frame  = b.data().subspan(message_info.offset, b.data().size() - message_info.offset);
        const unsigned char* header = frame.data();
            uint32_t             size   = *(const uint32_t*)&header[4];
        // logger.info("received payload size {}", );
            if (write(video_tunnel_out, frame.data() + 8, size) == -1) {
          logger.error("fail to write to video out channel");
        }
      })) {
        logger.warning("failed to dispatch frame writer task");
      }
   }
  }

  void on_new_symbol(dvb_slot_symbol_point symbol_point) override
  {
    if (!tx_executor.execute([this, b = std::move(symbol_point)]() mutable { send_dvb_frame(std::move(b)); })) {
       logger.warning("Failed to dispatch send task");
    }
    if (symbol_point.get_symbol_index() == 8) {
      if (!prepare_executor.execute([this, b = std::move(symbol_point)]() mutable { prepare_dvb_frame(std::move(b)); })) {
        logger.warning("Failed to dispatch prepare task");
      }
    }
  }

  void send_dvb_frame(dvb_slot_symbol_point symbol_point) {
    static_vector<span<const uint8_t>, MAX_BURST_SIZE> frame_burst;
    uint8_t frame_index = symbol_point.get_frame();
    // get frame buff
    auto& eaxc_frames = test_frame_data[frame_index & 1];
    uint8_t symbol = symbol_point.get_symbol_index();
    // send one symbol date
    // Set correct header parameters and send UL U-Plane packets for each symbol.
    if (symbol >= MAX_SEND_SYMBOLS - 1) {
      return;
    }
    if (symbol >= eaxc_frames.size()) {
      logger.info("data prepare too late on frame {} symbol {}", frame_index, symbol);
      return;
    }
    auto& symbol_frames = eaxc_frames[symbol];
    // Set runtime header parameters.
    for (auto& frame : symbol_frames) {
      frame_burst.emplace_back(frame.data(), frame.size());
    }

    // Send symbols.
    transceiver.send(frame_burst);

    // Increment TX_TOTAL counter.
    tx_total_counter.increment(frame_burst.size());
  }

  void prepare_dvb_frame(dvb_slot_symbol_point symbol_point) {
    uint8_t frame_index = symbol_point.get_frame();
    // prepare next frame data
    generate_test_frame_data(frame_index + 1);
  }

  void start()
  {
    // prepare first test_frame
    generate_test_frame_data(0);
    transceiver.start(*this);
  }

  void print_statistics(unsigned emu_id)
  {
    fmt::memory_buffer buffer;

    auto    now          = std::chrono::system_clock::now();
    std::tm current_time = fmt::gmtime(std::chrono::system_clock::to_time_t(now));
    uint64_t rx_total  = rx_total_counter.get_value();
    uint64_t tx_total  = tx_total_counter.get_value();
    uint64_t malformed = corrupt_counter.get_value();
    uint64_t dropped   = dropped_counter.get_value();

    fmt::format_to(buffer,
                   "| {:%H:%M:%S} | {:^3} | {:^11} | {:^11} | {:^11} | {:^11} |\n",
                   current_time,
                   emu_id,
                   rx_total,
                   malformed,
                   dropped,
                   tx_total);

    fmt::print(to_c_str(buffer));
  }

  std::vector<dvb_symbol_boundary_notifier*> get_ota_notifiers()
  {
    std::vector<dvb_symbol_boundary_notifier*> notifiers;
    notifiers.push_back(this);
    return notifiers;
  }

  void save_frame()
  {
    need_save_frame = true;
  }
private:

  /// Decodes and processes received OFH message.
  void process_new_frame(unique_rx_buffer buffer)
  {

  }

  void set_static_header_params(span<uint8_t> frame, header_parameters &head_param)
  {
    units::bytes  ether_hdr_size  = eth_builder->get_header_size();
    eth_builder->build_frame(frame);
    if (head_param.last_pkg) {
      struct dvb_transport_extend_header_t* dvb_head = (struct dvb_transport_extend_header_t*)(frame.data() + ether_hdr_size.value());
      dvb_head->frame_id = head_param.frame_id;
      dvb_head->payload = htons(head_param.payload_size);
      dvb_head->block_number = htons(head_param.nof_prbs);
      dvb_head->dvb_master_id = 6;
      dvb_head->start_block = htonl(head_param.start_prb);
      dvb_head->version = 1;
      dvb_head->seqid = htons(seq_counters[0]++);
      dvb_head->last_pkg_flag = 1;
      dvb_head->ef = 1;
      dvb_head->scale_factor = 0;
      dvb_head->modcod = 1;
    } else {
      struct dvb_transport_header_t* dvb_head = (struct dvb_transport_header_t*)(frame.data() + ether_hdr_size.value());
      dvb_head->frame_id = head_param.frame_id;
      dvb_head->payload = htons(head_param.payload_size);
      dvb_head->block_number = htons(head_param.nof_prbs);
      dvb_head->dvb_master_id = 6;
      dvb_head->start_block = htonl(head_param.start_prb);
      dvb_head->version = 1;
      dvb_head->seqid = htons(seq_counters[0]++);
    }
  }

  void build_dvb_frame(span<uint8_t>& frame, uint8_t frame_id, unsigned data_size, unsigned start_prb, unsigned number_prb, bool last_pkg)
  {
    unsigned dvb_header_size = last_pkg ? sizeof(dvb_transport_extend_header_t) : sizeof(dvb_transport_header_t);
    unsigned header_size = eth_builder->get_header_size().value();
    // Prepare header.
    span<uint8_t>     frame_header = frame.subspan(0, header_size + dvb_header_size);
    header_parameters params;
    params.frame_id = frame_id;
    params.payload_size = data_size + dvb_header_size;
    params.start_prb    = start_prb;
    params.nof_prbs     = number_prb;
    params.last_pkg = last_pkg;

    set_static_header_params(frame_header, params);

    // Prepare IQ data.
    char* data_buf = (char*)frame.subspan(header_size + dvb_header_size, data_size).data();
    ssize_t bytes_read = read(video_tunnel_in, data_buf + 8, data_size - 8);
    if (bytes_read >0) {
      // logger.info("read {} bytes from video tunnel", bytes_read);
      data_buf[0] = 'A';
      data_buf[1] = 'B';
      data_buf[2] = 'C';
      data_buf[3] = 'D';
      *(uint32_t*)&data_buf[4] = bytes_read;
    }
  }

  /// Returns pre-generated test data for each symbol.
  void generate_test_frame_data(unsigned frame_id)
  {
    // Vector of bytes for each frame (up to 2) of each OFDM symbol of each eAxC.
    eaxc_buffers& eaxc_frames = test_frame_data[frame_id & 1];
    eaxc_frames.clear();

    const units::bytes dvb_header_size(sizeof(struct dvb_transport_header_t));
    const units::bytes dvb_ext_header_size(sizeof(struct dvb_transport_extend_header_t));
    const units::bytes ether_header_size(eth_builder->get_header_size());
    const unsigned rb_size = 4;

    unsigned headers_size = (ether_header_size + dvb_header_size).value();
    // Size in bytes of one PRB using the given static compression parameters.
    unsigned rbs_per_frame = (cfg.mtu - headers_size) / rb_size;

    // It is assumed that maximum 2 packets required to send symbol data for antenna.
    unsigned nof_frames = (cfg.nof_prb / rbs_per_frame) + ((cfg.nof_prb % rbs_per_frame) ? 1 : 0);

    // Initializes IQ data and Ethernet packet headers (timestamp and sequence index
    // will be updated on every transmission).
    unsigned nof_frames_persymbol = nof_frames / (MAX_SEND_SYMBOLS - 1);
    unsigned left_frame = nof_frames - nof_frames_persymbol * (MAX_SEND_SYMBOLS - 1);

    unsigned start_prb = 0;
    for (unsigned symbol = 0, end = MAX_SEND_SYMBOLS - 1; symbol != end; ++symbol) {
      eaxc_frames.emplace_back();
      auto& symbol_frames = eaxc_frames.back();
      unsigned max_frames = nof_frames_persymbol + ((left_frame > 1) ? 1 : 0);
      if (left_frame > 1) {
        left_frame--;
      }
      for (unsigned j = 0; j != max_frames; ++j) {
        unsigned data_size = rbs_per_frame * rb_size;
        symbol_frames.emplace_back();
        std::vector<uint8_t>& frame = symbol_frames.back();
        frame.resize(headers_size + data_size);
        span<uint8_t> span_frame(frame.data(), headers_size + data_size);
        build_dvb_frame(span_frame, frame_id, data_size, start_prb, rbs_per_frame, false);

        start_prb += rbs_per_frame;
      }
      if ((symbol == end - 1) && (start_prb < cfg.nof_prb)) {
        unsigned data_size = (cfg.nof_prb - start_prb) * rb_size;
        headers_size = (ether_header_size + dvb_ext_header_size).value();
        if (headers_size + data_size > cfg.mtu) {
          symbol_frames.emplace_back();
          std::vector<uint8_t>& frame = symbol_frames.back();
          data_size = ((cfg.nof_prb - start_prb) / 2) *rb_size;
          headers_size = (ether_header_size + dvb_header_size).value();
          frame.resize(headers_size + data_size);
          span<uint8_t> span_frame(frame.data(), headers_size + data_size);
          build_dvb_frame(span_frame, frame_id, data_size, start_prb, data_size / rb_size, false);
          start_prb += (cfg.nof_prb - start_prb) / 2;
          data_size = (cfg.nof_prb - start_prb) * rb_size;
          headers_size = (ether_header_size + dvb_ext_header_size).value();
        }
        symbol_frames.emplace_back();
        std::vector<uint8_t>& frame = symbol_frames.back();
        frame.resize(headers_size + data_size);
        span<uint8_t> span_frame(frame.data(), headers_size + data_size);
        build_dvb_frame(span_frame, frame_id, data_size, start_prb, data_size / rb_size, true);
      }
    }
  }
};

/// Manages the workers of the RU emulators.
struct worker_manager {
  static constexpr uint32_t task_worker_queue_size = 1024;

  worker_manager(unsigned nof_emulators) { create_executors(nof_emulators); }

  void create_executors(unsigned nof_emulators)
  {
    using namespace execution_config_helper;

    for (unsigned i = 0; i != nof_emulators; ++i) {
      // Executors for Open Fronthaul messages reception.
      {
        const std::string name      = "dvb_sim_rx_#" + std::to_string(i);
        const std::string exec_name = "dvb_sim_rx_exec_#" + std::to_string(i);

        const single_worker dvb_worker{name,
                                      {concurrent_queue_policy::lockfree_spsc, 2},
                                      {{exec_name}},
                                      std::chrono::microseconds{1},
                                      os_thread_realtime_priority::max() - 1};
        if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
          report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
        }
        dvb_rx_exec.push_back(exec_mng.executors().at(exec_name));
      }

      // Executors for the dvb send frame.
      {
        const std::string   name      = "dvb_tx_sim_#" + std::to_string(i);
        const std::string   exec_name = "dvb_tx_sim_exec_#" + std::to_string(i);
        const single_worker dvb_worker{name,
                                      {concurrent_queue_policy::lockfree_spsc, task_worker_queue_size},
                                      {{exec_name}},
                                      std::chrono::microseconds{1},
                                      os_thread_realtime_priority::max() - 1};
        if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
          report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
        }
        dvb_tx_sims_exec.push_back(exec_mng.executors().at(exec_name));
      }
      // Executors for the dvb prepare frame.
      {
        const std::string   name      = "dvb_prep_#" + std::to_string(i);
        const std::string   exec_name = "dvb_prep_exec_#" + std::to_string(i);
        const single_worker dvb_worker{name,
                                      {concurrent_queue_policy::lockfree_spsc, task_worker_queue_size},
                                      {{exec_name}},
                                      std::chrono::microseconds{1},
                                      os_thread_realtime_priority::max() - 2};
        if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
          report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
        }
        dvb_prepare_frame_exec.push_back(exec_mng.executors().at(exec_name));
      }
      // Executors for the dvb save frame.
      {
        const std::string   name      = "dvb_save_#" + std::to_string(i);
        const std::string   exec_name = "dvb_save_exec_#" + std::to_string(i);
        const single_worker dvb_worker{name,
                                      {concurrent_queue_policy::lockfree_spsc, task_worker_queue_size},
                                      {{exec_name}},
                                      std::chrono::microseconds{1},
                                      os_thread_realtime_priority::max() - 2};
        if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
          report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
        }
        dvb_save_frame_exec.push_back(exec_mng.executors().at(exec_name));
      }
    }

    // Timing executor.
    {
      const std::string name      = "dvb_sim_timing";
      const std::string exec_name = "dvb_sim_timing_exec";

      const single_worker dvb_worker{name,
                                    {concurrent_queue_policy::lockfree_spsc, 4},
                                    {{exec_name}},
                                    std::chrono::microseconds{1},
                                    os_thread_realtime_priority::max() - 0};
      if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
        report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
      }
      dvb_timing_exec = exec_mng.executors().at(exec_name);
    }
  }

  void stop() { exec_mng.stop(); }

  task_execution_manager exec_mng;
  task_executor*         dvb_timing_exec = nullptr;

  std::vector<task_executor*> dvb_rx_exec;
  std::vector<task_executor*> dvb_tx_sims_exec;
  std::vector<task_executor*> dvb_prepare_frame_exec;
  std::vector<task_executor*> dvb_save_frame_exec;
};

} // namespace

static std::string config_file;

/// Flag that indicates if the application is running or being shutdown.
static std::atomic<bool> is_app_running = {true};
/// Maximum number of configuration files allowed to be concatenated in the command line.
static constexpr unsigned MAX_CONFIG_FILES = 1;

/// Function to call when the application is interrupted.
static void interrupt_signal_handler()
{
  is_app_running = false;
}

/// Function to call when the application is going to be forcefully shutdown.
static void cleanup_signal_handler()
{
  srslog::flush();
}

int main(int argc, char** argv)
{
  // Set interrupt and cleanup signal handlers.
  register_interrupt_signal_handler(interrupt_signal_handler);
  register_cleanup_signal_handler(cleanup_signal_handler);

  // Setup and configure config parsing.
  CLI::App app("dvb tx simulator application");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);
  app.set_config("-c,", config_file, "Read config from file", false)->expected(1, MAX_CONFIG_FILES);

  dvb_tx_sim_appconfig dvb_tx_sim_parsed_cfg;
  // Configure CLI11 with the RU emulator application configuration schema.
  configure_cli11_with_dvb_tx_sim_appconfig_schema(app, dvb_tx_sim_parsed_cfg);

  // Parse arguments.
  CLI11_PARSE(app, argc, argv);

  // Set up logging.
  srslog::sink* log_sink = (dvb_tx_sim_parsed_cfg.log_cfg.filename == "stdout")
                               ? srslog::create_stdout_sink()
                               : srslog::create_file_sink(dvb_tx_sim_parsed_cfg.log_cfg.filename);
  if (log_sink == nullptr) {
    report_error("Could not create application main log sink.\n");
  }
  srslog::set_default_sink(*log_sink);
  srslog::init();

  srslog::basic_logger& logger = srslog::fetch_basic_logger("DVB_TX_SIM", false);
  logger.set_level(dvb_tx_sim_parsed_cfg.log_cfg.level);

#ifdef DPDK_FOUND
  bool uses_dpdk = dvb_tx_sim_parsed_cfg.dpdk_config.has_value();

  // Initialize DPDK EAL.
  std::unique_ptr<dpdk::dpdk_eal> eal;
  if (uses_dpdk) {
    // Prepend the application name in argv[0] as it is expected by EAL.
    eal = dpdk::create_dpdk_eal(std::string(argv[0]) + " " + dvb_tx_sim_parsed_cfg.dpdk_config->eal_args,
                                srslog::fetch_basic_logger("EAL", false));
    if (!eal) {
      report_error("Failed to initialize DPDK EAL\n");
    }
  }
#endif
  // Create workers and executors.
  worker_manager workers(1);

  // Set up DPDK transceivers and create RU emulators.
  std::vector<std::unique_ptr<dvb_tx_sim_transceiver>> transceivers;
  std::vector<std::unique_ptr<dvb_tx_sim>>             dvb_tx_sims;

  dvb_tx_sim_ofh_appconfig dvb_tx_sim_cfg = dvb_tx_sim_parsed_cfg.dvb_tx_sim_cfg;

#ifdef DPDK_FOUND
  if (uses_dpdk) {
    dpdk_port_config port_cfg;
    port_cfg.pcie_id                     = dvb_tx_sim_cfg.network_interface;
    port_cfg.mtu_size                    = units::bytes{dvb_tx_sim_cfg.mtu};
    port_cfg.is_promiscuous_mode_enabled = dvb_tx_sim_cfg.enable_promiscuous;
    auto ctx                             = dpdk_port_context::create(port_cfg);
    transceivers.push_back(std::make_unique<dpdk_transceiver>(logger, *workers.dvb_rx_exec[0], ctx));
  } else
#endif
  {
    gw_config cfg;
    cfg.interface                   = dvb_tx_sim_cfg.network_interface;
    cfg.mtu_size                    = units::bytes{dvb_tx_sim_cfg.mtu};
    cfg.is_promiscuous_mode_enabled = dvb_tx_sim_cfg.enable_promiscuous;
    if (!parse_mac_address(dvb_tx_sim_cfg.dst_mac_address, cfg.mac_dst_address)) {
      report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.dst_mac_address);
    }
    transceivers.push_back(std::make_unique<socket_transceiver>(logger, *workers.dvb_rx_exec[0], cfg));
  }

  dvb_tx_sim_config emu_cfg;

  emu_cfg.nof_prb = MAX_DVB_FRAME_SIZE / 4;
  emu_cfg.input_file = dvb_tx_sim_cfg.input_file;

  emu_cfg.vlan_tag     = dvb_tx_sim_cfg.vlan_tag;
  emu_cfg.mtu          = dvb_tx_sim_cfg.mtu;
  if (!parse_mac_address(dvb_tx_sim_cfg.src_mac_address, emu_cfg.src_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.src_mac_address);
  }
  if (!parse_mac_address(dvb_tx_sim_cfg.dst_mac_address, emu_cfg.dst_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.dst_mac_address);
  }

  dvb_tx_sims.push_back(std::make_unique<dvb_tx_sim>(
      logger, *workers.dvb_tx_sims_exec[0], *workers.dvb_prepare_frame_exec[0], *workers.dvb_save_frame_exec[0], *transceivers[0], emu_cfg));


  // Create timing worker.
  dvb_tx_sim_timing_notifier timing_notifier(logger, *workers.dvb_timing_exec, dvb_tx_sim_cfg.frame_period);

  // Subscribe RU emulator window checkers to the 'OTA symbol start' notifications.
  std::vector<dvb_symbol_boundary_notifier*> dvb_symbol_notifiers;
  for (auto& dvb : dvb_tx_sims) {
    auto dvb_ota_notifiers = dvb->get_ota_notifiers();
    dvb_symbol_notifiers.insert(dvb_symbol_notifiers.end(), dvb_ota_notifiers.begin(), dvb_ota_notifiers.end());
  }
  timing_notifier.subscribe(dvb_symbol_notifiers);

  // Start dvb emulators.
  timing_notifier.start();
  for (auto& dvb : dvb_tx_sims) {
    dvb->start();
  }
  fmt::print("Running. Waiting for incoming packets...\n");

  fmt::print("| {:^8} | {:^3} | {:^11} | {:^11} | {:^11} | {:^11} |\n",
             "TIME",
             "ID",
             "RX_TOTAL",
             "RX_CORRUPT",
             "RX_ERR_DROP",
             "TX_TOTAL");
  std::string input;
  while (is_app_running) {
    for (unsigned i = 0, e = dvb_tx_sims.size(); i != e; ++i) {
      dvb_tx_sims[i]->print_statistics(i);
    }
    std::cout << "dvb>";
    getline(std::cin, input);
    if (input == "save") {
      for (unsigned i = 0, e = dvb_tx_sims.size(); i != e; ++i) {
        dvb_tx_sims[i]->save_frame();
      }
    } else if (input == "exit") {
      is_app_running = false;
      break;
    }
  }

  timing_notifier.stop();
  for (auto& txrx : transceivers) {
    txrx->stop();
  }
  workers.stop();
  srslog::flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fmt::print("\nDU tx sim app stopped\n");

  return 0;
}
