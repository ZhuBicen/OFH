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

#include "dvb_frame_pool.h"
#include "dvb_tx_sim_appconfig.h"
#include "dvb_tx_sim_cli11_schema.h"
#include "dvb_tx_sim_timing_notifier.h"
#include "dvb_tx_sim_transceiver.h"
#include "helpers.h"
#include "kpi_counter.h"
#include "media_transmitter.h"
#include "packet_sender.h"
#include "scoped_frame_buffer.h"
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
#include <chrono>
#include <fcntl.h>
#include <queue>
#include <random>
#include <signal.h>
#include <srsran/adt/to_array.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DPDK_FOUND
#include "srsran/hal/dpdk/dpdk_eal_factory.h"
#endif
#include "srsran/support/srsran_assert.h"

using namespace srsran;
using namespace ofh;
using namespace ether;

/// Maximum number of symbols in a slot, considering normal cyclic prefix.
static constexpr size_t MAX_NOF_SYMBOLS = 16;

/// Depending on configured compression parameters one UL U-Plane message may occupy up to 2 Ethernet packets.
static constexpr size_t MAX_NOF_PACKETS_PER_UPLANE_MESSAGE = 100;

static constexpr unsigned MAX_SAVE_FRAME = 8;

static constexpr unsigned MAX_DVB_FRAME_SIZE = 451584;

static constexpr unsigned NOF_ETHERNET_FRAME_IN_AIR_FRAME = 3000;

#include <iomanip>
#include <sstream>
using namespace std::chrono;

std::string formatDataSpeed(double bps)
{
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2); // Set 2 decimal places

  if (bps >= 1'000'000'000) { // 1 Gbps = 10^9 bps
    ss << bps / 1'000'000'000 << " Gbps";
  } else if (bps >= 1'000'000) { // 1 Mbps = 10^6 bps
    ss << bps / 1'000'000 << " Mbps";
  } else if (bps >= 1'000) { // 1 Kbps = 10^3 bps
    ss << bps / 1'000 << " Kbps";
  } else {
    ss << bps << " bps";
  }

  return ss.str();
}

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

  unsigned    speed_factor;
  unsigned    initial_num_of_packet;
  unsigned    packet_delay_in_nano_seconds;
  bool        variable_mtu;
  std::string input_file;
  std::string output_file;
};

} // namespace

namespace {

/// RU emulator receives OFH traffic and replies with UL packets to a DU.
class dvb_tx_sim : public frame_notifier
{
  srslog::basic_logger&   logger;
  task_executor&          tx_executor;
  task_executor&          prepare_executor;
  dvb_tx_sim_transceiver& transceiver;
  task_executor&          save_executor;

  // Timing window checkers, store statistics of early/late/on-time packets.
  // RU emulator configuration.
  const dvb_tx_sim_config cfg;
  // Keeps track of last used seq_id for each eAxC.
  circular_map<unsigned, uint16_t, MAX_SUPPORTED_EAXC_ID_VALUE> seq_counters;

  // Other KPI counters.
  kpi_counter                           rx_total_counter;
  kpi_counter                           video_rx_total_counter;
  kpi_counter                           tx_total_counter;
  kpi_counter                           tx_bytes;
  kpi_counter                           corrupt_counter;
  kpi_counter                           dropped_counter;
  kpi_counter                           lantencies;
  std::unique_ptr<ether::frame_builder> eth_builder;
  unsigned                              nof_per_symbol;

  int64_t min_latency = std::numeric_limits<int64_t>::max();
  int64_t max_latency = 0;

  std::string      input_stream_file_name;
  std::string      output_stream_file_name;
  bool             need_save_frame;
  bool             start_save_frame;
  PacketQueue      packet_queue;
  MediaTransmitter media_transmitter;
  PacketSender     packet_sender;

public:
  dvb_tx_sim(srslog::basic_logger&   logger_,
             task_executor&          tx_executor_,
             task_executor&          prepare_executor_,
             dvb_tx_sim_transceiver& transceiver_,
             task_executor&          save_executor_,
             dvb_tx_sim_config       cfg_) :
    logger(logger_),
    tx_executor(tx_executor_),
    prepare_executor(prepare_executor_),
    transceiver(transceiver_),
    save_executor(save_executor_),
    cfg(cfg_),
    input_stream_file_name(cfg_.input_file),
    output_stream_file_name(cfg_.output_file),
    need_save_frame(false),
    start_save_frame(false),
    packet_queue(NOF_ETHERNET_FRAME_IN_AIR_FRAME),
    media_transmitter(logger_,
                      input_stream_file_name,
                      output_stream_file_name,
                      packet_queue,
                      prepare_executor_,
                      cfg_.speed_factor,
                      cfg_.initial_num_of_packet,
                      cfg_.mtu,
                      cfg_.variable_mtu,
                      corrupt_counter,
                      dropped_counter),
    packet_sender(logger_, tx_executor, transceiver_, packet_queue, tx_bytes, cfg_.packet_delay_in_nano_seconds)
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

    media_transmitter.set_eth_builder(eth_builder.get());
  }

  // See interface for documentation.
  void on_new_frame(unique_rx_buffer buffer) override
  {
    static unsigned save_received_frame = 0;
    static unsigned counter             = 0;
    auto recv_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    if (!save_executor.defer([this, b = std::move(buffer), recv_time] {
          size_t              ether_header_size = eth_builder->get_header_size().value();
          span<const uint8_t> frame = b.data().subspan(ether_header_size, b.data().size() - ether_header_size);
          // logger.info("Received new frame of size {}, payload {}", b.data().size(), frame.size());
          if (save_received_frame < 2) {
            save_to_binary_file(
                b.data().data(), b.data().size(), "received_frame_" + std::to_string(save_received_frame) + ".bin");
            save_received_frame++;
          }
          counter++;
          auto result = media_transmitter.forward_payload(frame);
          if (result == PayloadCheckResult::OK) {
            uint16_t seq       = get_packet_seq(b.data().data(), b.data().size());
            auto     send_time = packet_sender.get_send_time(seq);
            auto     latency   = (recv_time.time_since_epoch() - send_time.time_since_epoch()).count();
            if (latency < 0) {
              logger.warning("Negative latency detected: seq {}, {}, {} <-> {}",
                             seq,
                             latency,
                             recv_time.time_since_epoch(),
                             send_time.time_since_epoch());
              latency = 0;
            }
            if (latency < min_latency) {
              min_latency = latency;
            }
            if (latency > max_latency) {
              max_latency = latency;
            }
            latency = latency < 0 ? 0 : latency;
            lantencies.increment(latency);
            rx_total_counter.increment();
            video_rx_total_counter.increment();
          } else if (result == PayloadCheckResult::INVALID_SYNC_HEADER) {
            static int invalid_sync_header_num = 0;
            save_to_binary_file(b.data().data(),
                                b.data().size(),
                                "frame_invalid_sync_header_" + std::to_string(invalid_sync_header_num++) + ".bin");
          }
        })) {
      logger.warning("Failed to dispatch save task");
    }
  }

  void start()
  {
    transceiver.start(*this);
    media_transmitter.start();
    packet_sender.start();
  }

  void print_statistics(unsigned emu_id)
  {
    fmt::memory_buffer buffer;
    static auto        last_time = std::chrono::system_clock::now();

    auto   now              = std::chrono::system_clock::now();
    double seconds          = std::chrono::duration<double>(now - last_time).count();
    last_time               = now;
    std::tm  current_time   = fmt::gmtime(std::chrono::system_clock::to_time_t(now));
    uint64_t rx_total       = rx_total_counter.get_value();
    uint64_t malformed      = corrupt_counter.get_value();
    uint64_t dropped        = dropped_counter.get_value();
    uint64_t tx_bytes_total = tx_bytes.get_value();
    double   lantency       = 0;

    if (rx_total) {
      lantency = (double)lantencies.get_value() / rx_total;
    }

    fmt::format_to(buffer,
                   "| {:%H:%M:%S} | {:^3} | {:^11} | {:^11} | {:^11} | {:^11} | {:^8.2f} | {:^10} | {:^10} | \n",
                   current_time,
                   emu_id,
                   rx_total,
                   malformed,
                   dropped,
                   formatDataSpeed(tx_bytes_total * 8 / seconds),
                   lantency,
                   min_latency == std::numeric_limits<int64_t>::max() ? "N/A" : std::to_string(min_latency),
                   max_latency);

    max_latency = 0;
    min_latency = std::numeric_limits<int64_t>::max();

    fmt::print(to_c_str(buffer));
  }

  void save_frame() { need_save_frame = true; }

private:
  /// Decodes and processes received OFH message.
  void process_new_frame(unique_rx_buffer buffer) {}
};

/// Manages the workers of the RU emulators.
struct worker_manager {
  static constexpr uint32_t task_worker_queue_size = 1024;

  worker_manager() { create_executors(); }

  void create_executors()
  {
    using namespace execution_config_helper;

    {
      const std::string name      = "dvb_sim_rx#";
      const std::string exec_name = "dvb_sim_rx_exec_";

      const single_worker dvb_worker{name,
                                     {concurrent_queue_policy::lockfree_spsc, 2},
                                     {{exec_name}},
                                     std::chrono::microseconds{1},
                                     os_thread_realtime_priority::max() - 1};
      if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
        report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
      }
      dvb_rx_exec = exec_mng.executors().at(exec_name);
    }

    // Executors for the dvb save frame.
    {
      const std::string   name      = "dvb_save";
      const std::string   exec_name = "dvb_save_exec";
      const single_worker dvb_worker{name,
                                     {concurrent_queue_policy::lockfree_spsc, task_worker_queue_size},
                                     {{exec_name}},
                                     std::chrono::microseconds{1},
                                     os_thread_realtime_priority::max() - 2};
      if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
        report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
      }
      dvb_save_frame_exec = exec_mng.executors().at(exec_name);
    }

    // Packet sender executor.
    {
      const std::string name      = "dvb_packet_sender";
      const std::string exec_name = "dvb_packet_sender_exec";

      const single_worker dvb_worker{name,
                                     {concurrent_queue_policy::lockfree_spsc, 4},
                                     {{exec_name}},
                                     std::chrono::microseconds{1},
                                     os_thread_realtime_priority::max() - 0};
      if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
        report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
      }
      packet_sender_exec = exec_mng.executors().at(exec_name);
    }

    // Packet prepare executor.
    {
      const std::string name      = "dvb_packet_prepare";
      const std::string exec_name = "dvb_packet_prepare_exec";

      const single_worker dvb_worker{name,
                                     {concurrent_queue_policy::lockfree_spsc, 4},
                                     {{exec_name}},
                                     std::chrono::microseconds{1},
                                     os_thread_realtime_priority::max() - 0};
      if (!exec_mng.add_execution_context(create_execution_context(dvb_worker))) {
        report_fatal_error("Failed to instantiate {} execution context", dvb_worker.name);
      }
      packet_prepare_exec = exec_mng.executors().at(exec_name);
    }
  }

  void stop() { exec_mng.stop(); }

  task_execution_manager exec_mng;

  task_executor* dvb_rx_exec         = nullptr;
  task_executor* dvb_save_frame_exec = nullptr;
  task_executor* packet_sender_exec  = nullptr;
  task_executor* packet_prepare_exec = nullptr;
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

void sigpipe_handler(int signo)
{
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
  worker_manager workers;

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
    transceivers.push_back(std::make_unique<dpdk_transceiver>(logger, *workers.dvb_rx_exec, ctx));
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
    transceivers.push_back(std::make_unique<socket_transceiver>(logger, *workers.dvb_rx_exec, cfg));
  }

  dvb_tx_sim_config emu_cfg;

  emu_cfg.nof_prb                      = MAX_DVB_FRAME_SIZE / 4;
  emu_cfg.input_file                   = dvb_tx_sim_cfg.input_file;
  emu_cfg.output_file                  = dvb_tx_sim_cfg.output_file;
  emu_cfg.speed_factor                 = dvb_tx_sim_cfg.speed_factor;
  emu_cfg.initial_num_of_packet        = dvb_tx_sim_cfg.initial_num_of_packet;
  emu_cfg.packet_delay_in_nano_seconds = dvb_tx_sim_cfg.packet_delay_in_nano_seconds;
  emu_cfg.vlan_tag                     = dvb_tx_sim_cfg.vlan_tag;
  emu_cfg.mtu                          = dvb_tx_sim_cfg.mtu;
  emu_cfg.variable_mtu                 = dvb_tx_sim_cfg.variable_mtu;
  if (!parse_mac_address(dvb_tx_sim_cfg.src_mac_address, emu_cfg.src_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.src_mac_address);
  }
  if (!parse_mac_address(dvb_tx_sim_cfg.dst_mac_address, emu_cfg.dst_mac)) {
    report_error("Invalid MAC address provided: '{}'", dvb_tx_sim_cfg.dst_mac_address);
  }
  logger.info("input video tunnel {}", dvb_tx_sim_cfg.input_file);
  logger.info("output video tunnel {}", dvb_tx_sim_cfg.output_file);
  logger.info("------------------------------------------");
  logger.info("variable mtu? {}", emu_cfg.variable_mtu);
  logger.info("initial_num_of_packet {}", emu_cfg.initial_num_of_packet);
  logger.info("packet_delay_in_nano_seconds {}", emu_cfg.packet_delay_in_nano_seconds);
  logger.info("------------------------------------------");

  // Create timing worker.

  dvb_tx_sims.push_back(std::make_unique<dvb_tx_sim>(logger,
                                                     *workers.packet_sender_exec,
                                                     *workers.packet_prepare_exec,
                                                     *transceivers[0],
                                                     *workers.dvb_save_frame_exec,
                                                     emu_cfg));

  for (auto& dvb : dvb_tx_sims) {
    dvb->start();
  }
  fmt::print("Running. Waiting for incoming packets...\n");

  fmt::print("| {:^8} | {:^3} | {:^11} | {:^11} | {:^11} | {:^11} |{:^10} |{:^10} |{:^10} |\n",
             "TIME",
             "ID",
             "RX_TOTAL",
             "TX_VIDEO",
             "TX_DUMMY",
             "BITRATE",
             "Avg(us)",
             "Min(us)",
             "Max(us)");
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

  // timing_notifier.stop();
  for (auto& txrx : transceivers) {
    txrx->stop();
  }
  workers.stop();
  srslog::flush();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  fmt::print("\nDU tx sim app stopped\n");

  return 0;
}
