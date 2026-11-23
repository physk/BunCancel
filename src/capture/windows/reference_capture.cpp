#include "reference_capture.hpp"

#include "packet_queue.hpp"
#include "sample_format.hpp"

#include <atomic>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <initguid.h>

#include <audioclient.h>
#include <propkeydef.h>

#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propvarutil.h>
#endif

namespace buncancel::capture {

namespace {
constexpr std::size_t queue_capacity = 64;

#ifdef _WIN32
template <typename T> class ComPointer {
public:
  ~ComPointer() {
    if (pointer_) {
      pointer_->Release();
    }
  }
  T *get() const noexcept { return pointer_; }
  T *operator->() const noexcept { return pointer_; }
  T **put() noexcept { return &pointer_; }

private:
  T *pointer_{};
};

class Apartment {
public:
  Apartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~Apartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }
  bool usable() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

private:
  HRESULT result_;
};

class EventHandle {
public:
  explicit EventHandle(bool manual_reset)
      : handle_(CreateEventW(nullptr, manual_reset, FALSE, nullptr)) {}
  ~EventHandle() {
    if (handle_) {
      CloseHandle(handle_);
    }
  }
  HANDLE get() const noexcept { return handle_; }

private:
  HANDLE handle_{};
};

std::string utf8(const wchar_t *text) {
  if (!text || *text == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                          result.data(), size, nullptr, nullptr) == 0) {
    return {};
  }
  result.pop_back();
  return result;
}

std::wstring utf16(const std::string &text) {
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(),
                          size) == 0) {
    return {};
  }
  return result;
}

bool parse_format(const WAVEFORMATEX &wave, detail::SampleFormat &format) {
  format.channels = wave.nChannels;
  format.sample_rate = wave.nSamplesPerSec;
  format.bits = wave.wBitsPerSample;
  format.valid_bits = wave.wBitsPerSample;
  if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    format.encoding = detail::Encoding::floating_point;
  } else if (wave.wFormatTag == WAVE_FORMAT_PCM) {
    format.encoding = detail::Encoding::signed_pcm;
  } else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             wave.cbSize >=
                 sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave);
    constexpr GUID float_format{STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
    constexpr GUID pcm_format{STATIC_KSDATAFORMAT_SUBTYPE_PCM};
    if (IsEqualGUID(extended.SubFormat, float_format)) {
      format.encoding = detail::Encoding::floating_point;
    } else if (IsEqualGUID(extended.SubFormat, pcm_format)) {
      format.encoding = detail::Encoding::signed_pcm;
    } else {
      return false;
    }
    format.valid_bits = extended.Samples.wValidBitsPerSample;
  } else {
    return false;
  }
  return detail::valid_format(format) &&
         wave.nBlockAlign == format.channels * (format.bits / 8) &&
         wave.nAvgBytesPerSec == wave.nSamplesPerSec * wave.nBlockAlign;
}

class MixFormat {
public:
  ~MixFormat() { CoTaskMemFree(value); }
  WAVEFORMATEX *value{};
};

class ClientStop {
public:
  explicit ClientStop(IAudioClient *client) : client_(client) {}
  ~ClientStop() { client_->Stop(); }

private:
  IAudioClient *client_;
};
#endif
} // namespace

struct ReferenceCapture::Impl {
  detail::PacketQueue<ReferencePacket, queue_capacity> queue;
  std::atomic<CaptureStatus> status{CaptureStatus::stopped};
  std::atomic<std::uint64_t> generation{};
  std::atomic<std::uint64_t> dropped{};
  std::atomic<std::uint64_t> discontinuities{};
  std::atomic<std::uint32_t> actual_rate{};
  std::atomic<bool> stop_requested{};
  std::thread worker;
#ifdef _WIN32
  std::unique_ptr<EventHandle> stop_event;

  void run(const CaptureConfig &config, std::uint64_t stream_generation) {
    Apartment apartment;
    ComPointer<IMMDeviceEnumerator> enumerator;
    ComPointer<IMMDevice> device;
    ComPointer<IMMEndpoint> endpoint;
    ComPointer<IAudioClient> client;
    ComPointer<IAudioCaptureClient> capture;
    const auto endpoint_id = utf16(config.endpoint_id);
    const auto fail = [this](CaptureStatus reason) {
      status.store(reason, std::memory_order_release);
    };
    if (!apartment.usable() || endpoint_id.empty() ||
        FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(enumerator.put()))) ||
        FAILED(enumerator->GetDevice(endpoint_id.c_str(), device.put())) ||
        FAILED(device->QueryInterface(
            __uuidof(IMMEndpoint),
            reinterpret_cast<void **>(endpoint.put())))) {
      fail(CaptureStatus::device_error);
      return;
    }
    EDataFlow flow{};
    const auto expected_flow =
        config.kind == EndpointKind::render_loopback ? eRender : eCapture;
    if (FAILED(endpoint->GetDataFlow(&flow)) || flow != expected_flow ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(client.put())))) {
      fail(CaptureStatus::device_error);
      return;
    }
    MixFormat mix;
    detail::SampleFormat format;
    if (FAILED(client->GetMixFormat(&mix.value)) || !mix.value ||
        !parse_format(*mix.value, format)) {
      fail(CaptureStatus::unsupported_format);
      return;
    }
    actual_rate.store(format.sample_rate, std::memory_order_release);
    // Resampling/drift correction is not implemented here. Refuse an unsafe
    // timebase instead of treating differing-rate samples as aligned evidence.
    if (format.sample_rate != config.sample_rate) {
      fail(CaptureStatus::rate_mismatch);
      return;
    }
    EventHandle data_event(false);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        (flow == eRender ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    if (!data_event.get() ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                  mix.value, nullptr)) ||
        FAILED(client->SetEventHandle(data_event.get())) ||
        FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void **>(capture.put()))) ||
        FAILED(client->Start())) {
      fail(CaptureStatus::device_error);
      return;
    }
    ClientStop client_stop(client.get());
    status.store(CaptureStatus::running, std::memory_order_release);
    bool discontinuity = true;
    bool first_buffer = true;
    const HANDLE events[]{stop_event->get(), data_event.get()};
    while (!stop_requested.load(std::memory_order_acquire)) {
      const auto wait = WaitForMultipleObjects(2, events, FALSE, 250);
      if (wait == WAIT_OBJECT_0) {
        return;
      }
      if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) {
        fail(CaptureStatus::device_error);
        return;
      }
      UINT32 available{};
      if (FAILED(capture->GetNextPacketSize(&available))) {
        fail(CaptureStatus::device_error);
        return;
      }
      while (available != 0 &&
             !stop_requested.load(std::memory_order_acquire)) {
        BYTE *data{};
        UINT32 frames{};
        DWORD packet_flags{};
        UINT64 device_position{};
        UINT64 qpc_position{};
        if (FAILED(capture->GetBuffer(&data, &frames, &packet_flags,
                                      &device_position, &qpc_position))) {
          fail(CaptureStatus::device_error);
          return;
        }
        if ((packet_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ||
            (!first_buffer &&
             (packet_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))) {
          discontinuities.fetch_add(1, std::memory_order_relaxed);
        }
        first_buffer = false;
        discontinuity = discontinuity ||
                        (packet_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
        const bool timestamp_valid =
            !(packet_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
        const bool silent = (packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (!timestamp_valid || (!silent && !data)) {
          discontinuity = true;
        } else {
          for (UINT32 offset = 0; offset < frames;) {
            ReferencePacket packet;
            packet.frame_count =
                std::min(ReferencePacket::max_frames, frames - offset);
            packet.channel_count = format.channels;
            packet.sample_rate = format.sample_rate;
            packet.device_position = device_position + offset;
            packet.qpc_position_100ns =
                qpc_position +
                static_cast<UINT64>(offset) * 10000000ULL / format.sample_rate;
            packet.generation = stream_generation;
            packet.discontinuity = discontinuity;
            if (!silent) {
              const auto sample_bytes = format.bits / 8;
              const auto sample_offset = offset * format.channels;
              const auto sample_count = packet.frame_count * format.channels;
              for (UINT32 sample = 0; sample < sample_count; ++sample) {
                packet.samples[sample] = detail::decode_sample(
                    data + (sample_offset + sample) * sample_bytes, format);
              }
            }
            if (queue.push(packet)) {
              discontinuity = false;
            } else {
              dropped.fetch_add(1, std::memory_order_relaxed);
              discontinuity = true;
            }
            offset += packet.frame_count;
          }
        }
        if (FAILED(capture->ReleaseBuffer(frames)) ||
            FAILED(capture->GetNextPacketSize(&available))) {
          fail(CaptureStatus::device_error);
          return;
        }
      }
    }
  }
#endif
};

ReferenceCapture::ReferenceCapture() : impl_(std::make_unique<Impl>()) {}

ReferenceCapture::~ReferenceCapture() { stop(); }

std::vector<Endpoint> ReferenceCapture::enumerate_endpoints() {
  std::vector<Endpoint> result;
#ifdef _WIN32
  Apartment apartment;
  ComPointer<IMMDeviceEnumerator> enumerator;
  if (!apartment.usable() ||
      FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(enumerator.put())))) {
    return result;
  }
  for (const auto flow : {eRender, eCapture}) {
    ComPointer<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              devices.put()))) {
      continue;
    }
    UINT count{};
    if (FAILED(devices->GetCount(&count))) {
      continue;
    }
    for (UINT index = 0; index < count; ++index) {
      ComPointer<IMMDevice> device;
      ComPointer<IPropertyStore> properties;
      LPWSTR id{};
      if (FAILED(devices->Item(index, device.put())) ||
          FAILED(device->GetId(&id))) {
        continue;
      }
      Endpoint entry;
      entry.id = utf8(id);
      CoTaskMemFree(id);
      entry.kind = flow == eRender ? EndpointKind::render_loopback
                                   : EndpointKind::capture;
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
          entry.name = utf8(name.pwszVal);
        }
        PropVariantClear(&name);
      }
      if (!entry.id.empty()) {
        if (entry.name.empty()) {
          entry.name = "Audio endpoint";
        }
        result.push_back(std::move(entry));
      }
    }
  }
#endif
  return result;
}

bool ReferenceCapture::start(const CaptureConfig &config) {
  stop();
  impl_->actual_rate.store(0, std::memory_order_release);
  if (config.endpoint_id.empty() || config.sample_rate < 8000 ||
      config.sample_rate > 192000) {
    impl_->status.store(CaptureStatus::device_error, std::memory_order_release);
    return false;
  }
#ifdef _WIN32
  impl_->stop_event = std::make_unique<EventHandle>(true);
  if (!impl_->stop_event->get()) {
    impl_->status.store(CaptureStatus::device_error, std::memory_order_release);
    return false;
  }
  impl_->stop_requested.store(false, std::memory_order_release);
  const auto generation =
      impl_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  impl_->status.store(CaptureStatus::starting, std::memory_order_release);
  try {
    impl_->worker = std::thread([this, config, generation] {
      try {
        impl_->run(config, generation);
      } catch (...) {
        impl_->status.store(CaptureStatus::device_error,
                            std::memory_order_release);
      }
    });
  } catch (...) {
    impl_->status.store(CaptureStatus::device_error, std::memory_order_release);
    return false;
  }
  return true;
#else
  impl_->status.store(CaptureStatus::device_error, std::memory_order_release);
  return false;
#endif
}

void ReferenceCapture::stop() noexcept {
  impl_->stop_requested.store(true, std::memory_order_release);
  impl_->generation.fetch_add(1, std::memory_order_acq_rel);
#ifdef _WIN32
  if (impl_->stop_event) {
    SetEvent(impl_->stop_event->get());
  }
#endif
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->status.store(CaptureStatus::stopped, std::memory_order_release);
#ifdef _WIN32
  impl_->stop_event.reset();
#endif
}

bool ReferenceCapture::pop(ReferencePacket &packet) noexcept {
  if (impl_->status.load(std::memory_order_acquire) != CaptureStatus::running) {
    return false;
  }
  const auto generation = impl_->generation.load(std::memory_order_acquire);
  for (std::size_t discarded = 0; discarded < queue_capacity; ++discarded) {
    if (!impl_->queue.pop(packet)) {
      return false;
    }
    if (packet.generation == generation) {
      return impl_->generation.load(std::memory_order_acquire) == generation &&
             impl_->status.load(std::memory_order_acquire) ==
                 CaptureStatus::running;
    }
  }
  return false;
}

CaptureStatus ReferenceCapture::status() const noexcept {
  return impl_->status.load(std::memory_order_acquire);
}

std::uint64_t ReferenceCapture::dropped_packets() const noexcept {
  return impl_->dropped.load(std::memory_order_relaxed);
}

std::uint64_t ReferenceCapture::discontinuities() const noexcept {
  return impl_->discontinuities.load(std::memory_order_relaxed);
}

std::uint32_t ReferenceCapture::actual_sample_rate() const noexcept {
  return impl_->actual_rate.load(std::memory_order_acquire);
}

} // namespace buncancel::capture
