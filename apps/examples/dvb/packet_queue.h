#pragma once

#include "srsran/adt/concurrent_queue.h"
#include "srsran/adt/span.h"
#include "srsran/adt/spsc_queue.h"

#include <memory>
#include <stdint.h>
#include <vector>

namespace srsran {
using Packet = std::shared_ptr<std::vector<uint8_t>>;
using PacketQueue =
    concurrent_queue<Packet, concurrent_queue_policy::lockfree_spsc, concurrent_queue_wait_policy::non_blocking>;
} // namespace srsran