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
#include "dvb_frame_pool.h"
#include "scoped_frame_buffer.h"
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
#include <random>
#include <srsran/adt/to_array.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <queue>

#ifdef DPDK_FOUND
#include "srsran/hal/dpdk/dpdk_eal_factory.h"
#endif
#include "srsran/support/srsran_assert.h"

using namespace srsran;
using namespace ofh;
using namespace ether;

/// Ethernet packet size.
static constexpr unsigned ETHERNET_FRAME_SIZE = 2048;

/// Maximum number of symbols in a slot, considering normal cyclic prefix.
static constexpr size_t MAX_NOF_SYMBOLS = 16;

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
  std::string output_file;
};

/// One symbol may require up to two byte buffers depending on configured compression parameters.
using symbol_buffer = static_vector<std::vector<uint8_t>, MAX_NOF_PACKETS_PER_UPLANE_MESSAGE>;

/// Array of symbol buffers, representing symbols of one eAxC.
using eaxc_buffers = static_vector<symbol_buffer, MAX_NOF_SYMBOLS>;

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
  dvb_tx_sim_timing_notifier& notifier;

  std::queue<srsran::ether::frame_buffer*> prepared_frames;

  // Timing window checkers, store statistics of early/late/on-time packets.
  // RU emulator configuration.
  const dvb_tx_sim_config cfg;
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
  unsigned nof_per_symbol;

  std::string input_stream_file_name;
  std::string output_stream_file_name;
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
              dvb_tx_sim_timing_notifier& notifier_,
              dvb_tx_sim_config       cfg_) :
    logger(logger_),
    tx_executor(tx_executor_),
    prepare_executor(prepare_executor_),
    save_executor(save_executor_),
    transceiver(transceiver_),
    notifier(notifier_),
    cfg(cfg_),
    input_stream_file_name(cfg_.input_file),
    output_stream_file_name(cfg_.output_file),
    video_tunnel_in(-1),
    video_tunnel_out(-1),
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

    open_video_tunnel_in();
    open_video_tunnel_out();
  }

  bool open_video_tunnel_in() {
    if (video_tunnel_in != -1) {
      close(video_tunnel_in);
      video_tunnel_in = -1;
    }
    if (mkfifo(input_stream_file_name.c_str(), 0666) == -1 && errno != EEXIST) {
      logger.error("failed to create input file. Error {}", strerror(errno));
      return false;
    }
    video_tunnel_in = open(input_stream_file_name.c_str(), O_RDONLY | O_NONBLOCK);
    return video_tunnel_in != -1;
  }


  bool open_video_tunnel_out()
  {
    if (video_tunnel_out != -1) {
      close(video_tunnel_out);
      video_tunnel_out = -1;
    }
    if (mkfifo(output_stream_file_name.c_str(), 0666) == -1 && errno != EEXIST) {
      logger.error("failed to create video out fifo. Error {}", strerror(errno));
      return false;
    }
    video_tunnel_out = open(output_stream_file_name.c_str(), O_WRONLY | O_NONBLOCK);
    if (video_tunnel_out == -1) {
      // logger.warning("Failed to open out video tunnel. Error: {}", strerror(errno));
      return false;
    }
    logger.info("open video tunnel out successful");
    // change_fifo_buffer_size(video_tunnel_out);
    return true;
  }

  // See interface for documentation.
  void on_new_frame(unique_rx_buffer buffer) override
  {
    rx_total_counter.increment();
    if (!save_executor.defer([this, b = std::move(buffer)] {
          size_t ether_header_size = eth_builder->get_header_size().value();
          span<const uint8_t> frame = b.data().subspan(ether_header_size, b.data().size() - ether_header_size);
          if (video_tunnel_out != -1) {
            if (write(video_tunnel_out, (char*)frame.data(), frame.size()) != (ssize_t)frame.size()) {
              logger.error("Failed to write to video tunnel out. Error: {}", strerror(errno));
              return;
            }
          }
        })) {
      logger.warning("Failed to dispatch save task");
    }
  }

  void on_new_symbol(dvb_slot_symbol_point symbol_point) override
  {
    if (!tx_executor.execute([this, b = std::move(symbol_point)]() mutable { prepare_dvb_frame(); send_dvb_frame(); })) {
       logger.warning("Failed to dispatch send task");
    }
    // if (symbol_point.get_symbol_index() == 8) {
    //   if (!prepare_executor.execute([this, b = std::move(symbol_point)]() mutable { prepare_dvb_frame(std::move(b)); })) {
    //     logger.warning("Failed to dispatch prepare task");
    //   }
    // }
  }

  void send_dvb_frame() {
    static_vector<span<const uint8_t>, MAX_BURST_SIZE> frame_burst;
    std::queue<srsran::ether::frame_buffer*> frames_to_send;
    for (unsigned int i = 0; i < MAX_BURST_SIZE; i++) {
      if (prepared_frames.empty()) {
        break;
      }
      auto frame = prepared_frames.front();
      frame_burst.emplace_back(frame->data());
      prepared_frames.pop();
      frames_to_send.push(frame);
    }
    transceiver.send(frame_burst);
    tx_total_counter.increment(frame_burst.size());

    while(!frames_to_send.empty()) {
      auto frame = frames_to_send.front();
      frames_to_send.pop();
      delete frame;
    }
  }

  void prepare_dvb_frame() {
    generate_test_frame_data();
  }

  void start()
  {
    // prepare first test_frame
    generate_test_frame_data();
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


  void generate_test_frame_data()
  {
    for(unsigned int i = 0; i < MAX_BURST_SIZE; i++) {
      auto bytes_read = 0;
      if (video_tunnel_in != -1) {
        srsran::ether::frame_buffer* frame = new srsran::ether::frame_buffer(ETHERNET_FRAME_SIZE);
        units::bytes  ether_hdr_size  = eth_builder->get_header_size();
        size_t payload_size = frame->size() - ether_hdr_size.value();

        eth_builder->build_frame(frame->data());

        bytes_read = read(video_tunnel_in, (char*)frame->data().subspan(ether_hdr_size.value(), payload_size).data(), payload_size);
        if (bytes_read <= 0) {
          if (bytes_read == 0) {
            logger.info("Video tunnel in closed, waiting for new data...");
            std::this_thread::sleep_for(std::chrono::seconds(2));
          }
          delete frame;
          continue;
        } else {
          prepared_frames.push(frame);
        }
      }
      if (bytes_read == -1 || video_tunnel_in == -1) {
        if (!open_video_tunnel_in()) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          logger.error("Failed to open video tunnel in. Error: {}", strerror(errno));
        }
        continue;
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

void sigpipe_handler(int signo) {
  fprintf(stderr, "SIGPIPE received: Reader closed FIFO.\n");
  // You might want to set a flag or perform cleanup here
  // For this example, we'll just print a message.
}

int main(int argc, char** argv)
{
  ::signal(SIGPIPE, sigpipe_handler);

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
  std::cout << "Parsing command line arguments..." << std::endl;
  CLI11_PARSE(app, argc, argv);
  std::cout << "Command line arguments parsed." << std::endl;

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
  emu_cfg.output_file = dvb_tx_sim_cfg.output_file;

  emu_cfg.vlan_tag     = dvb_tx_sim_cfg.vlan_tag;
  emu_cfg.mtu          = dvb_tx_sim_cfg.mtu;
  if (!parse_mac_address(dvb_tx_sim_cfg.src_mac_address, emu_cfg.src_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.src_mac_address);
  }
  if (!parse_mac_address(dvb_tx_sim_cfg.dst_mac_address, emu_cfg.dst_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.dst_mac_address);
  }
  // Create timing worker.
  dvb_tx_sim_timing_notifier timing_notifier(logger, *workers.dvb_timing_exec, dvb_tx_sim_cfg.frame_period);

  dvb_tx_sims.push_back(std::make_unique<dvb_tx_sim>(
      logger, *workers.dvb_tx_sims_exec[0], *workers.dvb_prepare_frame_exec[0], *workers.dvb_save_frame_exec[0], *transceivers[0], timing_notifier, emu_cfg));

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
