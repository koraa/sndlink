#include <unistd.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <array>
#include <atomic>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <portaudio.h>
#include <speex/speex_jitter.h>

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

uint64_t time_ms() {
  using namespace std::chrono;
  static auto t0 = high_resolution_clock::now();
  return duration_cast<milliseconds>(high_resolution_clock::now() - t0).count();
}

template<typename... Args>
void portaudio_ck(int code, Args&&... args) {
  check(code == paNoError, std::forward<Args>(args)..., Pa_GetErrorText(code));
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

boost::asio::io_service io_service;
std::array<uint8_t, frame_bytes + 8> framebuf;
uint64_t& frameno() {
  return *reinterpret_cast<uint64_t*>(std::data(framebuf));
}
uint8_t* frame_payload() {
  return std::data(framebuf) + 8;
}

void server_recv(udp::socket &sock, udp::endpoint &endp, JitterBuffer &jitbuf, std::mutex &mtx, std::atomic<uint64_t> &last_pkg_time) {
  sock.async_receive_from(boost::asio::buffer(framebuf), endp, [&](const auto &err, size_t) {
    if (err && err != boost::asio::error::message_size) return;
    {
      std::unique_lock<std::mutex> lck(mtx);
      JitterBufferPacket pkg{reinterpret_cast<char*>(frame_payload()), frame_bytes,
        static_cast<uint32_t>(frameno()*frame_stereo_samples), frame_stereo_samples,
        static_cast<uint16_t>(frameno()), 0};
      jitter_buffer_put(&jitbuf, &pkg);
    }
    last_pkg_time = time_ms();
    server_recv(sock, endp, jitbuf, mtx, last_pkg_time);
  });
}

void server(uint16_t port) {
  auto *jitbuf = jitter_buffer_init(frame_stereo_samples);
  std::mutex mtx;
  std::atomic<uint64_t> last_pkg_time;

  portaudio player{0, 2, [&](const void*, void *out, size_t) -> int {
    if (last_pkg_time == 0 || time_ms() - last_pkg_time > frame_ms*4) {
      ::memset(out, 0, frame_bytes);
      return ::paContinue;
    }
    std::unique_lock<std::mutex> lck(mtx);
    JitterBufferPacket pkg;
    pkg.data = (char*)out;
    pkg.len = frame_bytes;
    jitter_buffer_get(jitbuf, &pkg, frame_stereo_samples, nullptr);
    jitter_buffer_tick(jitbuf);
    return ::paContinue;
  }};

  udp::socket socket{io_service, udp::endpoint{udp::v6(), port}};
  socket.set_option(boost::asio::socket_base::reuse_address{true});
  socket.set_option(boost::asio::socket_base::receive_buffer_size(1000000));
  udp::endpoint endpoint;
  server_recv(socket, endpoint, *jitbuf, mtx, last_pkg_time);
  io_service.run();
}

void client(const char *ip, const char *port) {
  udp::resolver resolver(io_service);
  udp::resolver::query query(ip, port);
  udp::endpoint remote_endpoint = *resolver.resolve(query);

  udp::socket socket(io_service);
  socket.open(udp::v6());

  portaudio recorder{2, 0, [&](const void *inp, void*, size_t) -> int {
    frameno()++;
    ::memcpy(frame_payload(), inp, frame_bytes);

    socket.async_send_to(boost::asio::buffer(framebuf), remote_endpoint, [](const boost::system::error_code&, size_t) {
    });

    return ::paContinue;
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
    server(arg(2) == "" ? 47213 : atoi(argv[2]));
  } else {
    return usage();
  }
  return 0;
}
}
