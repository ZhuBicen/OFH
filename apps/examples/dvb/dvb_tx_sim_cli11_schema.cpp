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

#include "dvb_tx_sim_cli11_schema.h"
#include "dvb_tx_sim_appconfig.h"
#include "srsran/support/cli11_utils.h"
#include "srsran/support/config_parsers.h"

using namespace srsran;

static void configure_cli11_log_args(CLI::App& app, dvb_tx_sim_log_appconfig& log_params)
{
  /// Function to check that the log level is correct.
  auto check_log_level = [](const std::string& value) -> std::string {
    if (srslog::str_to_basic_level(value).has_value()) {
      return {};
    }

    return fmt::format("Log level '{}' not supported. Accepted values [none,info,debug,warning,error]", value);
  };
  /// Function to convert string parameter to srslog level.
  auto capture_log_level_function = [](srslog::basic_levels& level) {
    return [&level](const std::string& value) {
      auto val = srslog::str_to_basic_level(value);
      level    = (val) ? val.value() : srslog::basic_levels::none;
    };
  };

  app.add_option("--filename", log_params.filename, "Log file output path")->capture_default_str();
  add_option_function<std::string>(app, " --level", capture_log_level_function(log_params.level), "Log level")
      ->default_str(srslog::basic_level_to_string(log_params.level))
      ->check(check_log_level);
}

static void configure_cli11_dvb_tx_sim_dpdk_args(CLI::App& app, std::optional<dvb_tx_sim_dpdk_appconfig>& config)
{
  config.emplace();

  app.add_option("--eal_args", config->eal_args, "EAL configuration parameters used to initialize DPDK");
}

static void configure_cli11_dvb_tx_sim_args(CLI::App& app, dvb_tx_sim_ofh_appconfig& config)
{
  app.add_option("--gps_alpha", config.gps_Alpha, "GPS Alpha")
    ->capture_default_str()
    ->check(CLI::Range(0.0, 1.2288e7));
  app.add_option("--gps_beta", config.gps_Beta, "GPS Beta")->capture_default_str()->check(CLI::Range(-32768, 32767));
  app.add_option("--frame_period", config.frame_period, "Frame period")->capture_default_str();

  app.add_option("--network_interface", config.network_interface, "PCIe identifier of network device")
      ->capture_default_str();
  app.add_option("--src_mac_addr", config.src_mac_address, "Dvb tx Src MAC address")->capture_default_str();
  app.add_option("--dst_mac_addr", config.dst_mac_address, "Dvb tx Dst MAC address")->capture_default_str();
  app.add_option("--vlan_tag", config.vlan_tag, "V-LAN identifier")->capture_default_str()->check(CLI::Range(0, 4094));
  app.add_option("--mtu", config.mtu, "V-LAN identifier")->capture_default_str()->check(CLI::Range(2048, 9600));
  app.add_option("--enable_promiscuous", config.enable_promiscuous, "Promiscuous mode flag")->capture_default_str();
  app.add_option("--input_file", config.input_file, "input stream file")->capture_default_str();
  app.add_option("--output_file", config.output_file, "output stream file")->capture_default_str();
}

void srsran::configure_cli11_with_dvb_tx_sim_appconfig_schema(CLI::App& app, dvb_tx_sim_appconfig& dvb_tx_sim_parsed_cfg)
{
  // Logging section.
  CLI::App* log_subcmd = app.add_subcommand("log", "Logging configuration")->configurable();
  configure_cli11_log_args(*log_subcmd, dvb_tx_sim_parsed_cfg.log_cfg);

  // dvb tx sim section.
  CLI::App* ru_subcmd =
      app.add_subcommand("dvb_tx_sim", "Open Fronthaul Radio Unit emulator configuration")->configurable();

  // Cell parameters.
  ru_subcmd->add_option_function<std::string>(
      "--cells",
      [&dvb_tx_sim_parsed_cfg](const std::string& values) {
          CLI::App subapp("DVB tx simulators");
          subapp.config_formatter(create_yaml_config_parser());
          subapp.allow_config_extras(CLI::config_extras_mode::error);
          configure_cli11_dvb_tx_sim_args(subapp, dvb_tx_sim_parsed_cfg.dvb_tx_sim_cfg);
          std::istringstream ss(values);
          subapp.parse_from_stream(ss);
      },
      "Sets the dvb tx simulator configuration");

  CLI::App* dpdk_subcmd = app.add_subcommand("dpdk", "DPDK configuration")->configurable();
  configure_cli11_dvb_tx_sim_dpdk_args(*dpdk_subcmd, dvb_tx_sim_parsed_cfg.dpdk_config);

  app.callback([&]() {
    // Clean the DPDK optional.
    if (app.get_subcommand("dpdk")->count_all() == 0) {
      dvb_tx_sim_parsed_cfg.dpdk_config.reset();
    }
#ifdef DPDK_FOUND
    bool uses_dpdk = dvb_tx_sim_parsed_cfg.dpdk_config.has_value();
    if (uses_dpdk && dvb_tx_sim_parsed_cfg.dpdk_config->eal_args.empty()) {
      report_error("It is mandatory to fill the EAL configuration arguments to initialize DPDK correctly");
    }
#else
    if (dvb_tx_sim_parsed_cfg.dpdk_config.has_value()) {
      report_error("Unable to use DPDK as the application was not compiled with DPDK support");
    }
#endif
  });
}
