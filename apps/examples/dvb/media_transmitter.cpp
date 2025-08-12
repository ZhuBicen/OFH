#include "media_transmitter.h"

#include "crc16.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static constexpr unsigned ETHERNET_FRAME_SIZE = 2048;

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
#define DESIRED_PIPE_SIZE (8 * 1024 * 1024) // 4 MB
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

bool save_to_binary_file(const void* data_address, std::size_t data_length, const std::string& file_path)
{
  std::ofstream output_file(file_path, std::ios::out | std::ios::binary);

  if (!output_file.is_open()) {
    std::cerr << "Error: Could not open file " << file_path << " for writing." << std::endl;
    return false;
  }

  output_file.write(reinterpret_cast<const char*>(data_address), data_length);

  if (!output_file.good()) {
    std::cerr << "Error: Failed to write to file " << file_path << "." << std::endl;
    return false;
  }

  return true;
}

struct Header {
  uint32_t sync_header;
  uint16_t length;
  uint16_t sequence;
  uint16_t media_length;
} __attribute__((packed));

uint32_t SYNC_HEAD = 0x1ACFFC1D;

MediaTransmitter::MediaTransmitter(srslog::basic_logger&  logger_,
                                   const std::string&     input_stream,
                                   const std::string&     output_stream,
                                   srsran::PacketQueue&   packet_queue_,
                                   srsran::task_executor& executor_,
                                   uint16_t               speed_factor_) :
  packet_queue(packet_queue_),
  executor(executor_),
  input_stream_file_name(input_stream),
  output_stream_file_name(output_stream),
  video_tunnel_in(-1),
  video_tunnel_out(-1),
  logger(logger_),
  speed_factor(speed_factor_)
{
  // Initialize video tunnels
  if (!open_video_tunnel_in() || !open_video_tunnel_out()) {
    logger.error("Failed to open video tunnels");
  }
  logger.info("MediaTransmitter initialized with input: {}, output: {}, speed factor: {}",
              input_stream_file_name,
              output_stream_file_name,
              speed_factor);
}

MediaTransmitter::~MediaTransmitter()
{
  if (video_tunnel_in != -1) {
    close(video_tunnel_in);
  }
  if (video_tunnel_out != -1) {
    close(video_tunnel_out);
  }
}

static uint16_t get_crc(const span<const uint8_t>& payload)
{
  const Header* header = reinterpret_cast<const Header*>(payload.data());
  uint16_t      length = ntohs(header->length);
  return crc16_4bytes_optimized(payload.data() + sizeof(Header::sync_header), length, 0);
}

static void fill_dummy_payload(uint8_t* payload, size_t size)
{
  for (size_t i = 0; i < size; ++i) {
    payload[i] = 0x5a;
  }
}

void MediaTransmitter::start()
{
  logger.info("Starting media producer ...");
  std::promise<void> p;
  std::future<void>  fut = p.get_future();

  if (!executor.defer([this, &p]() {
        p.set_value();
        generate_media();
      })) {
    srsran::report_error("Unable to defer media generation task");
  }

  fut.wait();
  logger.info("Media producer started successfully");
}

void MediaTransmitter::generate_media()
{
  static bool save_first_packet = true;
  while (true) {
    if (video_tunnel_in == -1) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      open_video_tunnel_in();
      continue;
    }
    srsran::Packet packet = std::make_shared<std::vector<uint8_t>>(ETHERNET_FRAME_SIZE);
    packet->resize(ETHERNET_FRAME_SIZE, 0);
    size_t header_size = eth_builder->get_header_size().value();
    eth_builder->build_frame({packet->data(), packet->size()});

    size_t filled_size = 0;
    if (fill_payload({packet->data() + header_size, packet->size() - header_size}, filled_size, false)) {
      packet->resize(header_size + filled_size);
      if (save_first_packet) {
        save_to_binary_file(packet->data(), packet->size(), "first_send_packet.bin");
        save_first_packet = false;
      }
      if (!packet_queue.try_push(std::move(packet))) {
        logger.error("Failed to push media payload to packet queue, queue might be full");
      }
      // If speed factor is greater than 1, fill the payload with dummy data
      for (uint16_t i = 0; i < speed_factor - 1; ++i) {
        srsran::Packet packet2 = std::make_shared<std::vector<uint8_t>>(ETHERNET_FRAME_SIZE);
        packet2->resize(ETHERNET_FRAME_SIZE, 0);
        size_t header_size2 = eth_builder->get_header_size().value();
        eth_builder->build_frame({packet2->data(), packet2->size()});
        if (fill_payload({packet2->data() + header_size2, packet2->size() - header_size2}, filled_size, true)) {
          packet2->resize(header_size2 + filled_size);
          if (!packet_queue.try_push(std::move(packet2))) {
            logger.error("Failed to push media payload to packet queue, queue might be full");
          }
        } else {
          logger.error("Failed to fill payload with dummy data");
          break;
        }
      }
    }
  }
}

ssize_t fake_read(int fd, void* buf, size_t count)
{
  // Simulate reading data from a file descriptor
  fill_dummy_payload(static_cast<uint8_t*>(buf), count);
  return static_cast<ssize_t>(count);
}

bool MediaTransmitter::fill_payload(span<uint8_t> payload, size_t& payload_size, bool dummy)
{
  static bool print_first_header = true;
  if (video_tunnel_in == -1) {
    return false;
  }
  srsran_assert(payload.size() > sizeof(Header) + sizeof(uint16_t), "Payload size must be larger than Header size");

  span<uint8_t> media = payload.subspan(sizeof(Header), payload.size() - sizeof(Header) - sizeof(uint16_t));

  ssize_t bytes_read = 0;
  if (dummy) {
    bytes_read = fake_read(video_tunnel_in, media.data(), media.size());
  } else {
    bytes_read = read(video_tunnel_in, media.data(), media.size());
  }
  if (bytes_read == 0) {
    // logger.info("No data read from video tunnel in, possibly EOF or no data available.");
    payload_size = 0;
    open_video_tunnel_in();
    return false;
  }
  uint16_t media_size = 0;
  if (bytes_read < 0) {
    return false;
  } else {
    media_size = dummy ? 0 : static_cast<uint16_t>(bytes_read);
  }

  struct Header header;
  header.sync_header      = htonl(SYNC_HEAD);
  uint16_t length         = sizeof(header.sequence) + sizeof(header.media_length) + static_cast<uint16_t>(bytes_read);
  uint16_t padding_length = 0;
  if (length < 38) {
    padding_length = 38 - length;
  }
  header.length       = htons(length + padding_length);
  header.sequence     = htons(sequence_id);
  header.media_length = htons(media_size);
  memcpy(payload.data(), &header, sizeof(header));

  sequence_id = (sequence_id + 1) % UINT16_MAX;

  const uint16_t crc = htons(get_crc(payload));
  memcpy(payload.data() + sizeof(header) + bytes_read + padding_length, &crc, sizeof(crc));

  if (print_first_header) {
    logger.info(
        "Filling payload with header: sync_header=0x{:08X}, length={}, sequence={}, media_length={}, crc=0x{:04X}",
        ntohl(header.sync_header),
        ntohs(header.length),
        ntohs(header.sequence),
        ntohs(header.media_length),
        crc);
    print_first_header = false;
  }

  payload_size = static_cast<size_t>(sizeof(header) + bytes_read + padding_length + sizeof(crc));
  return true;
}

PayloadCheckResult MediaTransmitter::forward_payload(span<const uint8_t> payload)
{
  if (payload.size() < sizeof(Header) + sizeof(uint16_t)) {
    logger.error("Payload size is too small to contain header and CRC");
    return PayloadCheckResult::TOO_SMALL_PAYLOAD;
  }
  struct Header* original_header = (struct Header*)payload.data();
  struct Header  header          = *original_header;
  header.sync_header             = ntohl(original_header->sync_header);
  header.length                  = ntohs(original_header->length);
  header.sequence                = ntohs(original_header->sequence);
  header.media_length            = ntohs(original_header->media_length);
  uint16_t seq_id                = header.sequence;
  if (header.sync_header != SYNC_HEAD) {
    logger.error("Invalid sync header in payload");
    return PayloadCheckResult::INVALID_SYNC_HEADER;
  }
  if (header.length + sizeof(SYNC_HEAD) + sizeof(Header::length) + sizeof(uint16_t) != payload.size()) {
    logger.error("Payload length mismatch: expected {}, got {}",
                 header.length + sizeof(SYNC_HEAD) + sizeof(Header::length) + sizeof(uint16_t),
                 payload.size());
    return PayloadCheckResult::INVALID_LENGTH;
  }

  if (header.media_length > 0) {
    uint16_t expected_crc = htons(get_crc(payload));
    uint16_t received_crc = *(const uint16_t*)(payload.data() + payload.size() - 2);
    if (received_crc != expected_crc) {
      logger.error(
          "Payload CRC 0x{:04X}, expected 0x{:04X}, indicating a possible corruption", received_crc, expected_crc);
      return PayloadCheckResult::INVALID_CRC;
    }
  }
  std::vector<uint8_t> data(payload.data() + sizeof(Header), payload.data() + sizeof(Header) + header.media_length);
  packet_receiver.receive_packet(srsran::RxPacket(seq_id, std::move(data)));
  const auto& sorted_packets = packet_receiver.get_sorted_packets();
  for (const auto& rx_packet : sorted_packets) {
    if (rx_packet.data.size() != 0) {
      ssize_t bytes_written = write(video_tunnel_out, rx_packet.data.data(), rx_packet.data.size());
      if (bytes_written != header.media_length) {
        logger.error("Failed to write to video tunnel out. Error: {}", strerror(errno));
        return PayloadCheckResult::VIDEO_TUNNEL_BUSY;
      }
    }
  }

  return PayloadCheckResult::OK;
}

bool MediaTransmitter::open_video_tunnel_in()
{
  if (video_tunnel_in != -1) {
    close(video_tunnel_in);
    video_tunnel_in = -1;
  }
  if (mkfifo(input_stream_file_name.c_str(), 0666) == -1 && errno != EEXIST) {
    logger.error("failed to create input file. Error {}", strerror(errno));
    return false;
  }
  video_tunnel_in = open(input_stream_file_name.c_str(), O_RDONLY | O_NONBLOCK);
  if (video_tunnel_in != -1) {
    // logger.info("open video tunnel in successful {}, fd {}", input_stream_file_name, video_tunnel_in);
    return true;
  } else {
    logger.info("Failed to open video tunnel in. File: {}, Error: {}", input_stream_file_name, strerror(errno));
    return false;
  }
}

bool MediaTransmitter::open_video_tunnel_out()
{
  if (video_tunnel_out != -1) {
    close(video_tunnel_out);
    video_tunnel_out = -1;
  }
  if (mkfifo(output_stream_file_name.c_str(), 0666) == -1 && errno != EEXIST) {
    logger.error("failed to create video out fifo. File: {}, Error {}", output_stream_file_name, strerror(errno));
    return false;
  }
  video_tunnel_out = open(output_stream_file_name.c_str(), O_WRONLY | O_NONBLOCK);
  if (video_tunnel_out == -1) {
    // logger.warning("Failed to open out video tunnel. Error: {}", strerror(errno));
    return false;
  }
  logger.info("open video tunnel out successful");
  change_fifo_buffer_size(video_tunnel_out);
  return true;
}