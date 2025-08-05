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

#pragma once

#include "srsran/adt/span.h"
#include "srsran/adt/static_vector.h"
#include "srsran/ofh/ethernet/ethernet_properties.h"
#include "srsran/ofh/ofh_constants.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/units.h"
#include "dvb_tx_sim_timing_notifier.h"
#include <mutex>

namespace srsran {
  namespace ether {
  class frame_buffer_array;

  /// Storage for one Ethernet frame buffer.
  class frame_buffer
  {
    /// State of the buffer inside a buffer pool.
    enum class frame_buffer_status { free, reserved, used, marked_to_send };

    size_t               sz     = 0;
    frame_buffer_status  status = frame_buffer_status::free;
    std::vector<uint8_t> buffer;

    friend class frame_buffer_array;

  public:
    /// Constructors.
    frame_buffer() = default;
    explicit frame_buffer(unsigned size) : sz(size), buffer(size, 0) {}

    constexpr bool empty() const noexcept { return sz == 0; }

    constexpr size_t size() const noexcept { return sz; }

    constexpr void set_size(size_t new_size) noexcept
    {
      sz = new_size;
    }

    constexpr void clear() noexcept { sz = 0; }

    span<uint8_t>       data() noexcept { return {buffer.data(), sz}; }
    span<const uint8_t> data() const noexcept { return {buffer.data(), sz}; }
};

class frame_buffer_array
{
  /// Type of the buffer used in the pool to store Ethernet frames with a specific dvb packet.
  using storage_array_type = std::vector<frame_buffer>;

  /// Number of entries in the internal storage. Each entry comprises multiple frame buffers, the number of
  /// buffers in one entry is received at construction time.
  static constexpr size_t NUM_OF_ENTRIES = 2;

  /// Structure used to count number of written and read elements in the circular array.
  struct rd_wr_counter {
    unsigned count    = 0;
    unsigned boundary = 0;
    // Constructor receives upper boundary.
    explicit rd_wr_counter(unsigned b) : boundary(b) {}
    // Increment by N positions and wrap-around if needed
    void increment(unsigned n = 1) { count += n; }
    // Return counter value.
    unsigned value() const { return count % boundary; }
  };

public:
  struct used_buffer {
    frame_buffer*          buffer;
    dvb_slot_symbol_point timestamp;
  };

  // Constructor receives number of buffers stored/read at a time, reserves storage for all eAxCs.
  frame_buffer_array(unsigned nof_buffers_to_return, unsigned buffer_size, unsigned nof_antennas) :
    increment_quant(nof_buffers_to_return),
    storage_nof_buffers(nof_buffers_to_return * nof_antennas * NUM_OF_ENTRIES),
    buffers_array(storage_nof_buffers, frame_buffer{buffer_size}),
    write_position(storage_nof_buffers)
  {
    aux_array.reserve(storage_nof_buffers);
  }

  // Returns view over increment_quant buffers for writing if they are free, returns empty span otherwise.
  // The state of returned buffers is changed to 'reserved'.
  span<frame_buffer> reserve_buffers()
  {
    span<frame_buffer> wr_buffers(&buffers_array[write_position.value()], increment_quant);

    bool free = std::all_of(wr_buffers.begin(), wr_buffers.end(), [](const frame_buffer& buffer) {
      return buffer.status == frame_buffer::frame_buffer_status::free;
    });
    if (!free) {
      return {};
    }
    // Mark buffers as reserved.
    for (auto& buffer : wr_buffers) {
      buffer.status = frame_buffer::frame_buffer_status::reserved;
    }
    write_position.increment(increment_quant);
    return wr_buffers;
  }

  // Stores actually used buffers in a list of buffers ready for sending.
  // Unused buffers state is changed to 'free'.
  void push_buffers(span<frame_buffer> prepared_buffers, dvb_slot_symbol_point symbol_point)
  {
    for (auto& buffer : prepared_buffers) {
      if (buffer.empty()) {
        buffer.status = frame_buffer::frame_buffer_status::free;
      } else {
        buffer.status = frame_buffer::frame_buffer_status::used;
        used_buffers.push_back({&buffer, symbol_point});
      }
    }
  }

  // Changed state of sent buffers to 'free'.
  void clear_buffers()
  {
    for (auto& buffer : buffers_array) {
      if (buffer.status == frame_buffer::frame_buffer_status::marked_to_send) {
        buffer.status = frame_buffer::frame_buffer_status::free;
      }
    }
  }

  // Changed state of all 'used' buffers to 'free'.
  void reset_buffers()
  {
    for (auto& buffer : buffers_array) {
      if (buffer.status != frame_buffer::frame_buffer_status::marked_to_send &&
          buffer.status != frame_buffer::frame_buffer_status::reserved) {
        buffer.status = frame_buffer::frame_buffer_status::free;
      }
    }
    used_buffers.clear();
  }

  // Returns a vector of pointers to the buffers ready for sending.
  span<const frame_buffer*> find_buffers_ready_for_sending()
  {
    aux_array.clear();
    for (auto& used_buf : used_buffers) {
      used_buf.buffer->status = frame_buffer::frame_buffer_status::marked_to_send;
      aux_array.emplace_back(used_buf.buffer);
    }
    used_buffers.clear();
    return aux_array;
  }

  span<const used_buffer> get_prepared_buffers() const { return used_buffers; }

private:
  // Number of buffers accessed at a time.
  unsigned increment_quant;
  // Maximum number of buffers stored in the pool.
  unsigned storage_nof_buffers;
  // Data buffers.
  storage_array_type buffers_array;
  // Used buffers are added to this list.
  static_vector<used_buffer, 128> used_buffers;
  // Auxiliary array used as a list of ready-to-send buffers returned to a reader.
  std::vector<const frame_buffer*> aux_array;
  // Keeps track of the current write position.
  rd_wr_counter write_position;
};

/// Pool of Ethernet frames pre-allocated for each slot symbol.
class dvb_frame_pool
{
  /// Number of slots the pool can accommodate.
  static constexpr size_t NUM_SLOTS = 20L;

  /// Maximum number of entries contained by the pool, one entry per OFDM symbol, sized to accommodate 20 slots.
  static constexpr size_t NUM_ENTRIES = 16 * NUM_SLOTS;

  /// Number of symbols in an interval for which an auxiliary vector is pre-allocated to store buffer pointers.
  static constexpr size_t NUM_INTERVAL_SYMBOL = 16;

  /// Pool entry stores three circular arrays for every OFH type (DL C-Plane, UL C-Plane and U-Plane).
  struct pool_entry {
    /// Circular arrays of Ethernet frame buffers for each OFH type (DL C-Plane, UL C-Plane and U-Plane).
    std::vector<frame_buffer_array> buffers;

    pool_entry(unsigned mtu, unsigned num_of_frames)
    {
      // U-Plane storage.
      buffers.emplace_back(num_of_frames, mtu, ofh::MAX_NOF_SUPPORTED_EAXC);
    }

    /// Returns a view over next free frame buffers for a given OFH type, or an empty span if there is no free space.
    span<frame_buffer> reserve_buffers()
    {
      frame_buffer_array& entry_buf = buffers[0];
      span<frame_buffer>  buffs     = entry_buf.reserve_buffers();
      // Reset size of buffers before returning.
      for (auto& buf : buffs) {
        buf.clear();
      }
      return buffs;
    }

    /// Push span of ready buffers to the array associated with the given OFH type.
    void push_buffers(const dvb_slot_symbol_point symbol_point, span<frame_buffer> prepared_buffers)
    {
      frame_buffer_array& entry_buf = buffers[0];
      entry_buf.push_buffers(prepared_buffers, symbol_point);
    }

    void clear_buffers()
    {
      frame_buffer_array& entry_buf = buffers[0];
      entry_buf.clear_buffers();
    }

    void reset_buffers()
    {
      frame_buffer_array& entry_buf = buffers[0];;
      entry_buf.reset_buffers();
    }

    span<const frame_buffer_array::used_buffer> get_prepared_buffers() const
    {
      const frame_buffer_array& entry_buf = buffers[0];
      return entry_buf.get_prepared_buffers();
    }

    /// Returns a view over a next stored frame buffer for a given OFH type.
    span<const frame_buffer*> read_buffers()
    {
      frame_buffer_array& entry_buf = buffers[0];
      return entry_buf.find_buffers_ready_for_sending();
    }
  }; // End of pool_entry class.

  /// Returns pool entry for the given slot and symbol.
  const pool_entry& get_pool_entry(dvb_slot_symbol_point slot_point) const
  {
    unsigned sym_index  = (slot_point.get_frame() * NUM_SLOTS + slot_point.get_symbol_index()) % NUM_ENTRIES;
    return pool[sym_index];
  }
  /// Returns pool entry for the given slot and symbol.
  pool_entry& get_pool_entry(dvb_slot_symbol_point slot_point)
  {
    return const_cast<pool_entry&>(const_cast<const dvb_frame_pool*>(this)->get_pool_entry(slot_point));
  }

public:
  /// Constructor;
  dvb_frame_pool(unsigned mtu, unsigned num_of_frames) : pool(NUM_ENTRIES, {mtu, num_of_frames})
  {
    aux_array.reserve(ofh::MAX_NOF_SUPPORTED_EAXC * num_of_frames * NUM_INTERVAL_SYMBOL);
  }

  /// Returns data buffer from the pool for the given slot and symbol.
  span<frame_buffer> get_frame_buffers(const dvb_slot_symbol_point& slot_point)
  {
    pool_entry& p_entry = get_pool_entry(slot_point);
    // Acquire lock before accessing pool entry.
    std::lock_guard<std::mutex> lock(mutex);
    return p_entry.reserve_buffers();
  }

  /// Increments number of prepared Ethernet frames in the given slot symbol.
  void push_frame_buffers(const dvb_slot_symbol_point& symbol_point, span<frame_buffer> prepared_buffers)
  {
    pool_entry& p_entry = get_pool_entry(symbol_point);
    // Lock and update the pool entry.
    std::lock_guard<std::mutex> lock(mutex);
    p_entry.push_buffers(symbol_point, prepared_buffers);
  }

  /// Returns data buffers from the pool to a reader thread given a specific symbol context.
  span<const frame_buffer*> read_frame_buffers(const dvb_slot_symbol_point& symbol_point)
  {
    pool_entry& p_entry = get_pool_entry(symbol_point);
    // Acquire lock before accessing pool entry.
    std::lock_guard<std::mutex> lock(mutex);
    return p_entry.read_buffers();
  }

  /// Clears prepared Ethernet frame buffers for the given symbol context.
  void clear_sent_frame_buffers(const dvb_slot_symbol_point& symbol_point)
  {
    pool_entry& p_entry = get_pool_entry(symbol_point);
    // Acquire lock before accessing pool entry.
    std::lock_guard<std::mutex> lock(mutex);
    p_entry.clear_buffers();
  }

  /// Clears stored buffers associated with the given slot and logs the messages that could not be sent.
  void clear_downlink_frame(unsigned frame, srslog::basic_logger& logger)
  {
    // Lock before changing the pool entries.
    std::lock_guard<std::mutex> lock(mutex);

    dvb_slot_symbol_point symbol_point(frame * 16, 16);
    // Clear buffers with User-Plane messages.
    for (unsigned symbol = 0; symbol != 16; ++symbol) {
      symbol_point += 1;
      pool_entry& up_entry = get_pool_entry(symbol_point);

      auto dl_up_buffers = up_entry.get_prepared_buffers();
      for (const auto& used_buf : dl_up_buffers) {
        if (used_buf.timestamp.get_frame() == frame) {
          continue;
        }
        logger.warning(
            "Detected '{}' late downlink U-Plane messages in the transmitter queue for frame '{}', symbol '{}'",
            dl_up_buffers.size(),
            used_buf.timestamp.get_frame(),
            used_buf.timestamp.get_symbol_index());
        up_entry.reset_buffers();
        break;
      }
    }
  }

  /// Returns number of slots the pool can accommodate.
  size_t pool_size_in_slots() const { return NUM_SLOTS; }

private:
  /// Buffer pool.
  std::vector<pool_entry> pool;
  /// Mutex protecting buffers read/write counters.
  mutable std::mutex mutex;
  /// Auxiliary buffer.
  std::vector<const frame_buffer*> aux_array;
};
}
}