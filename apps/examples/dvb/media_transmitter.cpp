#include "media_transmitter.h"

#include "crc16.h"

#include <arpa/inet.h>
#include <array>
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

uint32_t SYNC_HEAD = 0x1ACFFC1D;

std::array<uint16_t, UINT16_MAX> g_dummy_packets_crc;

static uint16_t get_crc(const span<const uint8_t>& payload, uint16_t seq, bool dummy = false)
{
  if (dummy) {
    return g_dummy_packets_crc[seq];
  }
  return crc16_4bytes_optimized(payload.data(), payload.size(), 0);
}

static void fill_dummy_payload(uint8_t* payload, size_t size, uint16_t content)
{
  if (content == 0) {
    content = 0xFFFF;
  }
  uint16_t* ptr = (uint16_t*)payload;

  for (size_t i = 0; i < (size / 2); ++i) {
    ptr[i] = htons(content);
  }
  if (size % 2 != 0) {
    payload[size - 1] = htons(content) & 0xFF;
  }
}

void MediaTransmitter::create_dummy_ethernet_frame(uint16_t seq)
{
  dummy_ethernet_frame = std::make_shared<std::vector<uint8_t>>(ETHERNET_FRAME_SIZE);
  dummy_ethernet_frame->resize(ETHERNET_FRAME_SIZE, 0);
  size_t header_size = eth_builder->get_header_size().value();
  eth_builder->build_frame({dummy_ethernet_frame->data(), dummy_ethernet_frame->size()});
  span<uint8_t> payload(dummy_ethernet_frame->data() + header_size, dummy_ethernet_frame->size() - header_size);
  auto          h = fill_media(dummy_ethernet_frame, true, seq);
  // seq need to be adjusted
  fill_header(dummy_ethernet_frame, *h);
}

void MediaTransmitter::calcaute_dummy_packet_crc()
{
  size_t   eth_header = eth_builder->get_header_size().value();
  auto     header     = (struct Header*)(dummy_ethernet_frame->data() + eth_header);
  uint16_t length     = ntohs(header->length);
  std::cout << __FUNCTION__ << length << std::endl;

  span<uint8_t> crc_payload(dummy_ethernet_frame->data() + eth_header + sizeof(SYNC_HEAD),
                            span<uint8_t>::size_type(length + 2));
  for (int i = 0; i < UINT16_MAX; i++) {
    fill_dummy_packet_seq({dummy_ethernet_frame->data(), dummy_ethernet_frame->size()}, i);
    g_dummy_packets_crc[i] = get_crc(crc_payload, i, false);
    if (i % 10000 == 0) {
      logger.info("Calculating dummy packet seq {}, crc 0x{:04X}", i, g_dummy_packets_crc[i]);
    }
  }
}

MediaTransmitter::MediaTransmitter(srslog::basic_logger&  logger_,
                                   const std::string&     input_stream,
                                   const std::string&     output_stream,
                                   srsran::PacketQueue&   packet_queue_,
                                   srsran::task_executor& executor_,
                                   uint16_t               speed_factor_,
                                   uint16_t               initial_num_of_packet_,
                                   kpi_counter&           tx_video_packet_counter_,
                                   kpi_counter&           tx_dummy_packet_counter_) :
  packet_receiver(logger_),
  packet_queue(packet_queue_),
  executor(executor_),
  input_stream_file_name(input_stream),
  output_stream_file_name(output_stream),
  video_tunnel_in(-1),
  video_tunnel_out(-1),
  logger(logger_),
  speed_factor(speed_factor_),
  initial_num_of_packet(initial_num_of_packet_),
  tx_video_packet_counter(tx_video_packet_counter_),
  tx_dummy_packet_counter(tx_dummy_packet_counter_)
{
  // Initialize video tunnels
  if (!open_video_tunnel_in()) {
    logger.error("Failed to open video in tunnels");
  }
  while (!open_video_tunnel_out()) {
    logger.info("Failed to open video out tunnels, please start fifo_to_tcp program");
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  logger.info("MediaTransmitter initialized with input: {}, output: {}, speed factor: {}, initial packets: {}",
              input_stream_file_name,
              output_stream_file_name,
              speed_factor,
              initial_num_of_packet);
}

void MediaTransmitter::set_eth_builder(srsran::ether::frame_builder* eth_builder_)
{
  eth_builder     = eth_builder_;
  ether_head_size = eth_builder->get_header_size().value();
  create_dummy_ethernet_frame(0);
  logger.info("dummy packet created");
  calcaute_dummy_packet_crc();
  logger.info("dummy packet crc generated");
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

void MediaTransmitter::fill_dummy_packet_seq(span<uint8_t> payload, uint16_t seq)
{
  size_t   eth_header = eth_builder->get_header_size().value();
  uint16_t sequence   = htons(seq);
  memcpy(payload.data() + eth_header + sizeof(SYNC_HEAD) + sizeof(Header::length), &sequence, sizeof(seq));
}

void MediaTransmitter::push_dummy_packet()
{
  // static uint16_t save_first_dummy_packet = false;
  tx_dummy_packet_counter.increment();
  int seq = sequence_id;

  create_dummy_ethernet_frame(seq);
  auto p = std::make_shared<std::vector<uint8_t>>(*dummy_ethernet_frame.get());
  fill_dummy_packet_seq({p->data(), p->size()}, seq);
  sequence_id = (sequence_id + 1) % UINT16_MAX;
  fill_crc({p->data(), p->size()}, false);

  // if (save_first_dummy_packet < 220) {
  //   save_to_binary_file(p->data(), p->size(), "dummy_packet_" + std::to_string(seq) + ".bin");
  //   save_first_dummy_packet++;
  // }
  push_packet_to_send_queue(p);
}

void MediaTransmitter::push_packet_to_send_queue(srsran::Packet packet)
{
  for (;;) {
    if (packet_queue.try_push(packet)) {
      break;
    }
  }
}

void MediaTransmitter::fill_crc(span<uint8_t> payload, bool dummy)
{
  size_t   eth_header = eth_builder->get_header_size().value();
  auto     header     = (struct Header*)(payload.data() + eth_header);
  uint16_t length     = ntohs(header->length);
  uint16_t seq        = ntohs(header->sequence);
  uint16_t crc        = ntohs(
      get_crc({payload.data() + eth_header + sizeof(SYNC_HEAD), span<uint8_t>::size_type(length + 2)}, seq, dummy));
  memcpy(payload.data() + eth_header + sizeof(SYNC_HEAD) + sizeof(Header::length) + length, &crc, sizeof(crc));
}

void MediaTransmitter::generate_media()
{
  // static bool save_first_video_packet = true;
  for (int i = 0; i < initial_num_of_packet; i++) {
    push_dummy_packet();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  while (true) {
    if (video_tunnel_in == -1) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      open_video_tunnel_in();
      continue;
    }
    srsran::Packet packet = std::make_shared<std::vector<uint8_t>>(ETHERNET_FRAME_SIZE);
    packet->resize(ETHERNET_FRAME_SIZE, 0);
    eth_builder->build_frame({packet->data(), packet->size()});

    if (auto h = fill_media(packet, false, 0); h) {
      // if (h->length < 38) {
      //   h->length = 38;
      // }
      // packet->resize(ether_head_size + sizeof(SYNC_HEAD) + sizeof(Header::length) + h->length + sizeof(uint16_t),
      //                false);
      // h->sequence = sequence_id;
      // sequence_id = (sequence_id + 1) % UINT16_MAX;
      // fill_header(packet, *h);
      // fill_crc({packet->data(), packet->size()}, false);
      // if (save_first_video_packet) {
      //   save_to_binary_file(packet->data(), packet->size(), "first_send_packet.bin");
      //   save_first_video_packet = false;
      // }
      // push_packet_to_send_queue(packet);
      // tx_video_packet_counter.increment();
      for (uint16_t i = 0; i < 1; ++i) {
        push_dummy_packet();
      }
    }
  }
}

ssize_t fake_read(int fd, void* buf, size_t count, uint16_t seq)
{
  // Simulate reading data from a file descriptor
  fill_dummy_payload(static_cast<uint8_t*>(buf), count, seq);
  return static_cast<ssize_t>(count);
}

/**
 * didn't fill crc
 */
std::optional<Header> MediaTransmitter::fill_media(srsran::Packet packet, bool dummy, uint16_t seq)
{
  if (video_tunnel_in == -1) {
    return std::nullopt;
  }
  srsran_assert(packet->size() > ether_head_size + sizeof(Header) + sizeof(uint16_t),
                "Payload size must be larger than Header size");
  // media payload include media media data, not including media length
  span<uint8_t> media_payload(packet->data() + ether_head_size + sizeof(Header),
                              packet->size() - ether_head_size - sizeof(Header) - CRC_LENGTH);
  ssize_t       bytes_read;
  if (dummy) {
    bytes_read = fake_read(video_tunnel_in, media_payload.data(), media_payload.size(), seq);
  } else {
    bytes_read = read(video_tunnel_in, media_payload.data(), media_payload.size());
  }
  if (bytes_read == 0) {
    // logger.info("No data read from video tunnel in, possibly EOF or no data available.");
    open_video_tunnel_in();
    return std::nullopt;
  }
  if (bytes_read < 0) {
    return std::nullopt;
  }

  struct Header header;
  header.sync_header    = SYNC_HEAD;
  const uint16_t length = sizeof(header.sequence) + sizeof(header.media_length) + static_cast<uint16_t>(bytes_read);
  header.length         = length;
  header.sequence       = 0;
  header.media_length   = dummy ? 0 : bytes_read;
  return header;
}

static struct Header headerToNetworkByte(const struct Header& header)
{
  struct Header h;
  h.sync_header  = htonl(SYNC_HEAD);
  h.length       = htons(header.length);
  h.sequence     = htons(header.sequence);
  h.media_length = htons(header.media_length);
  return h;
}

void MediaTransmitter::fill_header(srsran::Packet packet, Header& header)
{
  auto h = headerToNetworkByte(header);
  memcpy(packet->data() + ether_head_size, &h, sizeof(h));
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
    save_to_binary_file(payload.data(), payload.size(), "mismatched_length.bin");
    return PayloadCheckResult::INVALID_LENGTH;
  }

  uint16_t expected_crc = htons(get_crc(
      {payload.data() + sizeof(SYNC_HEAD), span<uint8_t>::size_type(header.length + 2)}, header.sequence, false));
  uint16_t received_crc = *(const uint16_t*)(payload.data() + payload.size() - 2);
  if (received_crc != expected_crc) {
    logger.error("Payload {}, crc 0x{:04X}, expected crc 0x{:04X}, indicating a possible corruption",
                 seq_id,
                 received_crc,
                 expected_crc);
    return PayloadCheckResult::INVALID_CRC;
  }

  std::vector<uint8_t> data(payload.data() + sizeof(Header), payload.data() + sizeof(Header) + header.media_length);
  packet_receiver.receive_packet(srsran::RxPacket(seq_id, std::move(data)));
  const auto& sorted_packets = packet_receiver.get_sorted_packets();
  for (const auto& rx_packet : sorted_packets) {
    if (rx_packet.data.size() != 0) {
      ssize_t bytes_written = write(video_tunnel_out, rx_packet.data.data(), rx_packet.data.size());
      if (bytes_written != (ssize_t)rx_packet.data.size()) {
        logger.error("Failed to write to video tunnel out. Error: {}, write {}, expecte {}",
                     strerror(errno),
                     bytes_written,
                     rx_packet.data.size());
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
    logger.warning("Failed to open out video tunnel. Error: {}", strerror(errno));
    return false;
  }
  logger.info("open video tunnel out successful");
  change_fifo_buffer_size(video_tunnel_out);
  return true;
}