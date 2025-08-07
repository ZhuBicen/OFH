#include "media_transmitter.h"

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

bool MediaTransmitter::fill_payload(span<uint8_t> payload, size_t& payload_size)
{
  if (video_tunnel_in == -1) {
    return false;
  }

  ssize_t bytes_read = read(video_tunnel_in, payload.data(), payload.size());
  if (bytes_read < 0) {
    return false;
  }
  if (bytes_read == 0) {
    // logger.info("No data read from video tunnel in, possibly EOF or no data available.");
    payload_size = 0;
    open_video_tunnel_in();
    return false;
  }

  payload_size = static_cast<size_t>(bytes_read);
  return true;
}

bool MediaTransmitter::forward_payload(span<const uint8_t> payload)
{
  if (video_tunnel_out == -1) {
    return false;
  }

  ssize_t bytes_written = write(video_tunnel_out, payload.data(), payload.size());
  if (bytes_written != (ssize_t)payload.size()) {
    logger.error("Failed to write to video tunnel out. Error: {}", strerror(errno));
    return false;
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