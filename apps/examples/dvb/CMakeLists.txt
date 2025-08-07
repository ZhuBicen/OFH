#
# Copyright 2021-2025 Software Radio Systems Limited
#
# This file is part of srsRAN
#
# srsRAN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# srsRAN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# A copy of the GNU Affero General Public License can be found in
# the LICENSE file in the top-level directory of this distribution
# and at http://www.gnu.org/licenses/.
#

set_directory_properties(PROPERTIES LABELS "dvb")

set (SOURCES
        dvb_tx_sim_cli11_schema.cpp
        dvb_tx_sim_timing_notifier.cpp
        dvb_tx_sim_socket_transceiver.cpp
        media_transmitter.cpp
        dvb_tx_sim.cpp)
if (DPDK_FOUND)
    list(APPEND SOURCES dvb_tx_sim_dpdk_transceiver.cpp)
endif (DPDK_FOUND)

add_executable(dvb_tx_sim ${SOURCES})
target_include_directories(dvb_tx_sim PRIVATE ${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/external)
target_link_libraries(dvb_tx_sim srslog srsran_ofh srsran_phy_support srsran_support)

if (DPDK_FOUND)
    target_compile_options(dvb_tx_sim PRIVATE ${DPDK_CFLAGS})
    target_link_libraries(dvb_tx_sim hal_dpdk ${DPDK_LIBRARIES})
    add_definitions(-DDPDK_FOUND)
endif (DPDK_FOUND)

install(TARGETS dvb_tx_sim
        RUNTIME)
