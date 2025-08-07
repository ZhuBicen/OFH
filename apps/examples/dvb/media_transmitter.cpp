#include "media_transmitter.h"

#include "crc16.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

struct Header {
  uint32_t sync_header;
  uint16_t length;
  uint16_t sequence;
  uint16_t media_length;
} __attribute__((packed));

uint32_t SYNC_HEAD = 0x1ACFFC1D;

MediaTransmitter::MediaTransmitter(srslog::basic_logger& logger_,
                                   const std::string&    input_stream,
                                   const std::string&    output_stream) :
  input_stream_file_name(input_stream),
  output_stream_file_name(output_stream),
  video_tunnel_in(-1),
  video_tunnel_out(-1),
  logger(logger_)
{
  // Initialize video tunnels
  if (!open_video_tunnel_in() || !open_video_tunnel_out()) {
    logger.error("Failed to open video tunnels");
  }
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
    payload[i] = 0xa5;
  }
}

bool MediaTransmitter::fill_payload(span<uint8_t> payload, size_t& payload_size)
{
  static bool print_first_header = true;
  if (video_tunnel_in == -1) {
    return false;
  }
  srsran_assert(payload.size() > sizeof(Header) + sizeof(uint16_t), "Payload size must be larger than Header size");

  span<uint8_t> media      = payload.subspan(sizeof(Header), payload.size() - sizeof(Header) - sizeof(uint16_t));
  ssize_t       bytes_read = read(video_tunnel_in, media.data(), media.size());
  if (bytes_read == 0) {
    // logger.info("No data read from video tunnel in, possibly EOF or no data available.");
    payload_size = 0;
    open_video_tunnel_in();
    return false;
  }
  uint16_t media_size = 0;
  if (bytes_read < 0) {
    fill_dummy_payload(media.data(), media.size());
    bytes_read = media.size();
    media_size = 0;
  } else {
    media_size = static_cast<uint16_t>(bytes_read);
  }

  struct Header header;
  header.sync_header = htonl(SYNC_HEAD);
  header.length      = htons(sizeof(header.sequence) + sizeof(header.media_length) + static_cast<uint16_t>(bytes_read));
  header.sequence    = htons(sequence_id);
  header.media_length = htons(media_size);
  memcpy(payload.data(), &header, sizeof(header));

  sequence_id = (sequence_id + 1) % UINT16_MAX;

  const uint16_t crc = htons(get_crc(payload));
  memcpy(payload.data() + sizeof(header) + bytes_read, &crc, sizeof(crc));

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

  payload_size = static_cast<size_t>(sizeof(header) + bytes_read + sizeof(crc));
  return true;
}

bool MediaTransmitter::forward_payload(span<const uint8_t> payload)
{
  if (video_tunnel_out == -1) {
    return false;
  }
  if (payload.size() < sizeof(Header) + sizeof(uint16_t)) {
    logger.error("Payload size is too small to contain header and CRC");
    return false;
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
    return false;
  }
  if (!last_received_sequence_id) {
    last_received_sequence_id = seq_id;
  } else {
    uint16_t expected_sequence = (last_received_sequence_id.value() + 1) % UINT16_MAX;
    if (header.sequence != expected_sequence) {
      logger.warning("Received sequence ID {} does not match expected {}",
                     ntohs(header.sequence),
                     last_received_sequence_id.value());
      last_received_sequence_id = (uint16_t)header.sequence;
      return false;
    }
    last_received_sequence_id = seq_id;
  }

  if (header.length + sizeof(SYNC_HEAD) + sizeof(Header::length) + sizeof(uint16_t) != payload.size()) {
    logger.error("Payload length mismatch: expected {}, got {}",
                 header.length + sizeof(SYNC_HEAD) + sizeof(Header::length) + sizeof(uint16_t),
                 payload.size());
    return false;
  }
  uint16_t expected_crc = htons(get_crc(payload));
  uint16_t received_crc = *(const uint16_t*)(payload.data() + payload.size() - 2);
  if (received_crc != expected_crc) {
    logger.error(
        "Payload CRC 0x{:04X}, expected 0x{:04X}, indicating a possible corruption", received_crc, expected_crc);
    return false;
  }
  if (header.media_length > 0) {
    ssize_t bytes_written = write(video_tunnel_out, payload.data() + sizeof(Header), header.media_length);
    if (bytes_written != header.media_length) {
      logger.error("Failed to write to video tunnel out. Error: {}", strerror(errno));
      return false;
    }
  } else {
    // logger.warning("Media length is zero, nothing to write to video tunnel out");
  }
  return true;
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