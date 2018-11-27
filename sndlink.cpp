#include <unistd.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <array>
#include <atomic>
#include <thread>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <portaudio.h>
#include <speex/speex_jitter.h>
#include "opus/opus.h"
#include "opus/opus_types.h"

using namespace boost::asio::ip;
using boost::asio::ip::udp;
using boost::asio::ip::address;

namespace sndlink {

// Note: The APIs here use various units; we generally use
// mono samples (one sample_t), stereo samples (2 sample_t since we have two channels)
// and bytes.
// 1 stereo sample = 2 samples = 4 bytes
using sample_t = int16_t;
constexpr size_t samplerate = 48000;
constexpr size_t channels = 2;
constexpr size_t frame_ms = 5;
constexpr size_t frame_stereo_samples = samplerate*frame_ms/1000;
constexpr size_t frame_mono_samples = frame_stereo_samples * channels;
constexpr size_t frame_bytes = frame_mono_samples * sizeof(uint16_t);

template<typename... Args>
void check(bool ck, Args&&... args) {
  if (ck) return;
  std::cerr << "[FATAL] ";
  (std::cerr << ... << args) << std::endl;
  ::abort();
}

template<typename... Args>
int portaudio_ck(int code, Args&&... args) {
  check(code == paNoError, std::forward<Args>(args)..., Pa_GetErrorText(code));
  return code;
}

template<typename... Args>
int opus_ck(int code, Args&&... args) {
  check(code >= 0, std::forward<Args>(args)..., opus_strerror(code));
  return code;
}

uint64_t time_ms() {
  using namespace std::chrono;
  static auto t0 = high_resolution_clock::now();
  return duration_cast<milliseconds>(high_resolution_clock::now() - t0).count();
}

template<typename Fn>
auto exec(Fn fn) {
  return fn();
}

template<typename Fn>
struct portaudio {
  ::PaStream* pastream{nullptr};
  Fn fn;

  portaudio(int inp_channels, int out_channels, Fn fn_) : fn{fn_} {
    portaudio_ck(Pa_Initialize());
    auto callback = [](const void *inp, void *out,
          size_t frames_per_buf,
          const ::PaStreamCallbackTimeInfo*,
          const ::PaStreamCallbackFlags,
          void *sis) -> int {
      auto me  = reinterpret_cast<portaudio*>(sis);
      return me->fn(inp, out, frames_per_buf);
    };
    portaudio_ck(
      ::Pa_OpenDefaultStream(&pastream,
        inp_channels, out_channels, paInt16,
        samplerate, frame_stereo_samples,
        callback, this));
    portaudio_ck(::Pa_StartStream(pastream));
  }

  ~portaudio() {
    portaudio_ck(::Pa_CloseStream(pastream));
    portaudio_ck(::Pa_Terminate());
  }

  portaudio(portaudio&&) = delete;
  portaudio(const portaudio&) = delete;
  portaudio& operator=(portaudio&&) = delete;
  portaudio& operator=(const portaudio&) = delete;
};

struct raw_frame {
  std::atomic<bool> used;
  uint64_t frameno;
  std::array<sample_t, frame_mono_samples> payload;
};

struct opus_frame {
  size_t payload_len;
  uint64_t frameno;
  std::array<uint8_t, 1024 /* TODO: dynamically calculate? */> payload;

  uint8_t* data() { return reinterpret_cast<uint8_t*>(&frameno); }
  constexpr size_t size() const { return payload_len + sizeof(frameno); }
  void size(size_t val) {
    check(val > sizeof(frameno), "Underflow in opus frame!");
    payload_len = val - sizeof(frameno);
  }
};

// Single producer/single consumer poor-mans object pool/queue with no
// ordering guarantees
// Basically a Double buffer for encoded & raw frames, so we can still be encoding
// audio while we are still sending data…
template<typename T, size_t N>
struct obj_pool {
  std::condition_variable cv;
  std::mutex mtx;

  std::array<std::atomic<bool>, N> marks{}; // TODO: Atomic bitset?
  std::array<T, N> values{};

  obj_pool() {
    for (auto &u : marks) u = false;
  }

  size_t calc_off(const T &v) {
    size_t r = &v - &values[0];
    check(r < N, "Invalid object passed to mark_marked/unmarked");
    return r;
  }

  void mark(const T &v) {
    std::lock_guard lck{mtx};
    marks[calc_off(v)] = true;
    cv.notify_one();
  }

  void unmark(const T &v) {
    marks[calc_off(v)] = false;
  }

  T* try_get_marked() {
    for (size_t idx{0}; idx < N; idx++)
      if (marks[idx])
        return &values[idx];
    return nullptr;
  }

  T& wait_get_marked() {
    auto *r = try_get_marked();
    if (r != nullptr) return *r;

    std::unique_lock lk{mtx};
    while (r == nullptr) {
      cv.wait(lk);
      r = try_get_marked();
    }
    return *r;
  }

  T* try_get_unmarked() {
    for (size_t idx{0}; idx < N; idx++)
      if (!marks[idx])
        return &values[idx];
    return nullptr;
  }
};

struct server {
  std::atomic<uint64_t> last_pkg_time{0};

  std::mutex jitbuf_mtx;
  ::JitterBuffer *jitbuf = jitter_buffer_init(frame_stereo_samples);

  ::OpusDecoder *decoder = exec([]() {
    int err;
    auto *r = opus_decoder_create(samplerate, channels, &err);
    opus_ck(err, "Error creating opus decoder");
    return r;
  });

  boost::asio::io_service io_service;
  udp::endpoint endpoint;
  udp::socket socket;

  std::array<uint8_t, 4096> pkgbuf;

  server(uint16_t port)
      : socket{io_service, udp::endpoint{udp::v6(), port}} {
    socket.set_option(boost::asio::socket_base::reuse_address{true});
    socket.set_option(boost::asio::socket_base::receive_buffer_size(1000000));
    setup_recv();
  }

  void run() {
    portaudio player{0, 2, [this](auto&&... args) -> int {
      return this->on_playback(std::forward<decltype(args)>(args)...);
    }};
    io_service.run();
  }

  void setup_recv() {
    socket.async_receive_from(boost::asio::buffer(pkgbuf), endpoint, [this](auto&&... args) {
      this->on_recv(std::forward<decltype(args)>(args)...);
    });
  }

  void on_recv(const boost::system::error_code &err, const size_t len) {
    if (err && err != boost::asio::error::message_size) return;

    ssize_t payload_len = static_cast<ssize_t>(len) - 8;
    check(payload_len > 0, "Underflow in input packet");

    size_t frameno{0};
    static_assert(sizeof(frameno) == 8);
    std::copy_n(std::data(pkgbuf), 8, reinterpret_cast<uint8_t*>(&frameno));

    // Not using an extra thread for decoding here, because (1) asio packet
    // recv is not as time critical and (2) decoding is a lot faster than
    // encoding
    std::array<uint8_t, frame_bytes> decoded;
    opus_ck(
        opus_decode(decoder, std::data(pkgbuf)+8, static_cast<int32_t>(payload_len), reinterpret_cast<int16_t*>(std::data(decoded)), frame_stereo_samples, 0),
        "opus_decode()");

    {
      std::unique_lock<std::mutex> lck(jitbuf_mtx);
      JitterBufferPacket pkg{
        reinterpret_cast<char*>(std::data(decoded)), frame_bytes,
        static_cast<uint32_t>(frameno*frame_stereo_samples), frame_stereo_samples,
        static_cast<uint16_t>(frameno), 0};
      jitter_buffer_put(jitbuf, &pkg);
      last_pkg_time = time_ms();
    }

    setup_recv();
  }

  int on_playback(const void*, void *out, size_t) {
    if (last_pkg_time == 0 || time_ms() - last_pkg_time > frame_ms*4) {
      ::memset(out, 0, frame_bytes);
      return ::paContinue;
    }

    std::unique_lock<std::mutex> lck(jitbuf_mtx);
    JitterBufferPacket pkg;
    pkg.data = (char*)out;
    pkg.len = frame_bytes;
    jitter_buffer_get(jitbuf, &pkg, frame_stereo_samples, nullptr);
    jitter_buffer_tick(jitbuf);

    return ::paContinue;
  }
};

void client(const char *ip, const char *port) {
  int err;
  ::OpusEncoder *enc = opus_encoder_create(samplerate, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  opus_ck(err, "Error creating opus encoder");
  ::opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
  ::opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
  ::opus_encoder_ctl(enc, OPUS_SET_VBR(1));
  ::opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0));

  boost::asio::io_service io_service;
  udp::resolver resolver(io_service);
  udp::resolver::query query(ip, port);
  udp::endpoint remote_endpoint = *resolver.resolve(query);

  udp::socket socket(io_service);
  socket.open(udp::v6());

  // We use the marks for the encoder thread to wait on buffers
  // to become free, so – unintuitively – the following configuration
  // is used:
  obj_pool<raw_frame, 8> raw_frames; // marked = carries data
  obj_pool<opus_frame, 8> opus_frames; // marked = is empty
  for (auto &u : opus_frames.marks) u = true;

  portaudio recorder{2, 0, [&](const void *inp, void*, size_t) -> int {
    static size_t frameno{0};
    frameno++;

    auto *raw = raw_frames.try_get_unmarked();
    if (raw == nullptr) {
      std::cerr << "Skipping frame " << frameno << " because no raw"
        << "frame processing slot is available!" << std::endl;
      return ::paContinue;
    }

    raw->frameno = frameno;
    std::copy_n(reinterpret_cast<const uint8_t*>(inp), frame_bytes, reinterpret_cast<uint8_t*>(std::data(raw->payload)));
    raw_frames.mark(*raw);
    return ::paContinue;
  }};

  std::thread encoder{[&]() {
    while (true) {
      auto &raw = raw_frames.wait_get_marked();
      auto &opus = opus_frames.wait_get_marked();

      opus.frameno = raw.frameno;
      opus.payload_len = opus_ck(opus_encode(enc, std::data(raw.payload), frame_stereo_samples, std::data(opus.payload), std::size(opus.payload)));
      raw_frames.unmark(raw);
      opus_frames.unmark(opus);;

      socket.async_send_to(boost::asio::buffer(std::data(opus), std::size(opus)), remote_endpoint, [&](const boost::system::error_code&, size_t) {
        opus_frames.mark(opus);
      });
    }
  }};

  auto x = make_work_guard(io_service);
  io_service.run();
}

int usage() {
  std::cerr << "USAGE: sndlink server [PORT]\n"
    << "USAGE: sndlink client ADDRESS [PORT]\n";
  return 3;
}

extern "C" int main(int argc, const char **argv) {
  auto arg = [&](size_t idx) {
    return idx >= static_cast<size_t>(argc) ? std::string_view{} : std::string_view{argv[idx]};
  };

  if (arg(1) == "client") {
    if (argc < 3) return usage();
    client(argv[2], arg(3) == "" ? "47213" : argv[3]);
  } else if (arg(1) == "server") {
    server{static_cast<uint16_t>(arg(2) == "" ? 47213 : atoi(argv[2]))}.run();
  } else {
    return usage();
  }
  return 0;
}
}
