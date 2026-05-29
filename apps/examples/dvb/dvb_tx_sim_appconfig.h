/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
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

#pragma once

#include "srsran/ran/bs_channel_bandwidth.h"
#include "srsran/srslog/srslog.h"
#include <string>
#include <vector>

namespace srsran {

/// RU emulator OFH configuration parameters.
struct dvb_tx_sim_ofh_appconfig {
  /// GPS Alpha - Valid value range: [0, 1.2288e7].
  unsigned gps_Alpha = 0;
  /// GPS Beta - Valid value range: [-32768, 32767].
  int gps_Beta = 0;

  /// frame period - valid value range: [8541, 34614]
  unsigned int frame_period;

  /// Ethernet network interface name or PCI bus identifier.
  std::string network_interface;
  /// dvb tx sim src MAC address.
  std::string src_mac_address;
  /// dvb s2 Unit MAC address.
  std::string dst_mac_address;
  /// V-LAN Tag control information field.
  unsigned vlan_tag;
  unsigned mtu;
  /// Promiscuous mode flag.
  bool enable_promiscuous = false;
  /// input stream file
  std::string input_file;
  std::string output_file;
  bool    enable_check_crc                 = true;
  unsigned    initial_num_of_packet        = 220;
  unsigned    packet_delay_in_nano_seconds = 0;
  bool        variable_mtu                 = false;
  unsigned    bitrate                      = 100; // in Mbps
};

/// RU emulator logging parameters.
struct dvb_tx_sim_log_appconfig {
  /// Log level
  srslog::basic_levels level = srslog::basic_levels::info;
  /// Path to log file or "stdout" to print to console.
  std::string filename = "stdout";
};

/// DPDK configuration.
struct dvb_tx_sim_dpdk_appconfig {
  /// EAL configuration arguments.
  std::string eal_args;
};

/// RU emulator application configuration.
struct dvb_tx_sim_appconfig {
  /// Logging configuration.
  dvb_tx_sim_log_appconfig log_cfg;
  /// Individual RU emulators configurations.
  dvb_tx_sim_ofh_appconfig dvb_tx_sim_cfg = {};
  /// DPDK configuration.
  std::optional<dvb_tx_sim_dpdk_appconfig> dpdk_config;
};

} // namespace srsran
