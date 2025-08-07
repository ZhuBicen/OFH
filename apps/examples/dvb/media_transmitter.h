#pragma once

#include "srsran/adt/span.h"
#include "srsran/srslog/logger.h"

#include <stdint.h>
#include <optional>

using srsran::span;

class MediaTransmitter {
public:
    MediaTransmitter(srslog::basic_logger& logger, const std::string& input_stream, const std::string& output_stream);
    ~MediaTransmitter();
    
bool fill_payload(span<uint8_t> payload, size_t& payload_size);
bool forward_payload(span<const uint8_t> payload);

private:
    std::string input_stream_file_name;
    std::string output_stream_file_name;
    int video_tunnel_in;
    int video_tunnel_out;
    srslog::basic_logger& logger;
    uint16_t sequence_id = 0;
    std::optional<uint16_t> last_received_sequence_id;


    bool open_video_tunnel_in();
    bool open_video_tunnel_out();
    bool save_to_binary_file(const void* data_address, std::size_t data_length, const std::string& file_path);
};
