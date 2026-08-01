#include "haptics.hpp"
#include "config.hpp"

#include <Windows.h>
#include <audioclient.h>
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <setupapi.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

template <typename T> void release(T*& value) {
    if (value) value->Release();
    value = nullptr;
}

struct Voice {
    HapticEffect effect{};
    float strength{};
    uint64_t frame{};
};

struct IPolicyConfigVista : IUnknown {
    virtual HRESULT GetMixFormat(LPCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT GetDeviceFormat(LPCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT SetDeviceFormat(LPCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT GetProcessingPeriod(LPCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT SetProcessingPeriod(LPCWSTR, PINT64) = 0;
    virtual HRESULT GetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT SetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT GetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetDefaultEndpoint(LPCWSTR, ERole) = 0;
    virtual HRESULT SetEndpointVisibility(LPCWSTR, BOOL) = 0;
};

bool endpoint_is_present(LPCWSTR endpoint_id) {
    std::wstring instance = L"SWD\\MMDEVAPI\\";
    instance += endpoint_id;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found{};
    SP_DEVINFO_DATA data{sizeof(data)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        wchar_t id[512]{};
        if (SetupDiGetDeviceInstanceIdW(set, &data, id, 512, nullptr) &&
            _wcsicmp(id, instance.c_str()) == 0) {
            found = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool enable_endpoint(LPCWSTR endpoint_id) {
    static const CLSID clsid = {0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
    static const IID iid = {0x568b9108,0x44bf,0x40b4,{0x90,0x06,0x86,0xaf,0xe5,0xb5,0xa6,0x20}};
    IPolicyConfigVista* policy{};
    const HRESULT create = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, iid,
                                             reinterpret_cast<void**>(&policy));
    if (FAILED(create)) return false;
    const HRESULT result = policy->SetEndpointVisibility(endpoint_id, TRUE);
    policy->Release();
    return SUCCEEDED(result);
}

double duration(HapticEffect effect) {
    switch (effect) {
    case HapticEffect::MenuLeft:
    case HapticEffect::MenuRight: return 0.025;
    case HapticEffect::FootLeft:
    case HapticEffect::FootRight: return 0.050;
    case HapticEffect::EnemyHit: return 0.085;
    case HapticEffect::PlayerHit: return 0.220;
    }
    return 0.05;
}

void synthesize(const Voice& voice, double t, float& left, float& right) {
    const double d = duration(voice.effect);
    if (t >= d) return;
    const float envelope = static_cast<float>((1.0 - t / d) * voice.strength);
    float sample{};
    float balance{};
    switch (voice.effect) {
    case HapticEffect::MenuLeft:
    case HapticEffect::MenuRight:
        sample = static_cast<float>(std::sin(2.0 * kPi * 185.0 * t)) * envelope;
        balance = voice.effect == HapticEffect::MenuLeft ? -0.55f : 0.55f;
        break;
    case HapticEffect::FootLeft:
    case HapticEffect::FootRight:
        sample = static_cast<float>(0.72 * std::sin(2.0 * kPi * 82.0 * t) +
                                    0.28 * std::sin(2.0 * kPi * 155.0 * t)) * envelope;
        balance = voice.effect == HapticEffect::FootLeft ? -0.68f : 0.68f;
        break;
    case HapticEffect::EnemyHit:
        sample = static_cast<float>(0.62 * std::sin(2.0 * kPi * 118.0 * t) +
                                    0.38 * std::sin(2.0 * kPi * 61.0 * t)) * envelope;
        balance = 0.0f;
        break;
    case HapticEffect::PlayerHit: {
        const float grit = static_cast<float>(std::sin(2.0 * kPi * 47.0 * t) *
                            std::sin(2.0 * kPi * 173.0 * t));
        sample = (0.55f * static_cast<float>(std::sin(2.0 * kPi * 68.0 * t)) +
                  0.45f * grit) * envelope;
        balance = static_cast<float>(0.25 * std::sin(2.0 * kPi * 9.0 * t));
        break;
    }
    }
    left += sample * (balance > 0.0f ? 1.0f - balance : 1.0f);
    right += sample * (balance < 0.0f ? 1.0f + balance : 1.0f);
}

void write_sample(BYTE* frame, const WAVEFORMATEX* format, unsigned channel, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    const unsigned bytes = format->wBitsPerSample / 8;
    BYTE* target = frame + channel * bytes;
    bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        is_float = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    if (is_float && bytes == 4) {
        *reinterpret_cast<float*>(target) = value;
    } else if (bytes == 2) {
        *reinterpret_cast<int16_t*>(target) = static_cast<int16_t>(value * 32767.0f);
    } else if (bytes == 3) {
        const int32_t v = static_cast<int32_t>(value * 8388607.0f);
        target[0] = static_cast<BYTE>(v);
        target[1] = static_cast<BYTE>(v >> 8);
        target[2] = static_cast<BYTE>(v >> 16);
    } else if (bytes == 4) {
        *reinterpret_cast<int32_t*>(target) = static_cast<int32_t>(value * 2147483647.0f);
    }
}
}  // namespace

struct Haptics::Impl {
    std::mutex mutex;
    std::condition_variable ready_cv;
    std::vector<Voice> pending;
    std::thread worker;
    bool ready{};
    bool initialized{};
    bool reported_absence{};
    std::atomic_bool stop{};

    void audio_session(std::atomic_bool& public_active) {
        initialized = false;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator{};
        IMMDeviceCollection* devices{};
        IMMDevice* selected{};
        IAudioClient* client{};
        IAudioRenderClient* renderer{};
        WAVEFORMATEX* format{};
        HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &devices);
        UINT count{};
        if (SUCCEEDED(hr)) devices->GetCount(&count);
        for (UINT i = 0; i < count && !selected; ++i) {
            IMMDevice* device{};
            IPropertyStore* properties{};
            PROPVARIANT name;
            PropVariantInit(&name);
            LPWSTR endpoint_id{};
            WAVEFORMATEX* candidate_format{};
            IAudioClient* candidate_client{};
            DWORD device_state{};
            devices->Item(i, &device);
            device->GetState(&device_state);
            device->GetId(&endpoint_id);
            device->OpenPropertyStore(STGM_READ, &properties);
            if (properties) properties->GetValue(PKEY_Device_FriendlyName, &name);
            const std::wstring friendly = name.vt == VT_LPWSTR ? name.pwszVal : L"";
            const bool dualsense = friendly.find(L"DualSense") != std::wstring::npos ||
                                   friendly.find(L"Wireless Controller") != std::wstring::npos;
            if (dualsense && !reported_absence)
                log_line("Haptics: found endpoint '%ls' (state 0x%lX)",
                         friendly.c_str(), device_state);
            if (dualsense && device_state == DEVICE_STATE_DISABLED && endpoint_id &&
                endpoint_is_present(endpoint_id) && enable_endpoint(endpoint_id)) {
                log_line("Haptics: enabled the connected DualSense playback endpoint");
                Sleep(150);
                device->GetState(&device_state);
            }
            if (dualsense && device_state == DEVICE_STATE_ACTIVE &&
                SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                    nullptr, reinterpret_cast<void**>(&candidate_client))) &&
                SUCCEEDED(candidate_client->GetMixFormat(&candidate_format))) {
                log_line("Haptics: candidate endpoint '%ls', %u Hz, %u channels, %u-bit",
                         friendly.c_str(), candidate_format->nSamplesPerSec,
                         candidate_format->nChannels, candidate_format->wBitsPerSample);
                if (candidate_format->nChannels >= 4) {
                    selected = device;
                    selected->AddRef();
                    client = candidate_client;
                    candidate_client = nullptr;
                    format = candidate_format;
                    candidate_format = nullptr;
                }
            }
            if (candidate_format) CoTaskMemFree(candidate_format);
            release(candidate_client);
            PropVariantClear(&name);
            if (endpoint_id) CoTaskMemFree(endpoint_id);
            release(properties);
            release(device);
        }
        release(devices);
        release(enumerator);

        if (!selected) {
            if (!reported_absence)
                log_line("Haptics: no active four-channel DualSense playback endpoint found; waiting for USB reconnect");
            reported_absence = true;
        } else {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    0, 0, format, nullptr);
            if (SUCCEEDED(hr)) hr = client->SetEventHandle(event_handle);
            if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient),
                                                       reinterpret_cast<void**>(&renderer));
            if (SUCCEEDED(hr)) hr = client->Start();
            initialized = SUCCEEDED(hr);
            if (!initialized) log_line("Haptics: audio initialization failed (HRESULT 0x%08lX)", hr);
            else {
                log_line("Haptics: advanced PCM stream started on actuator channels 3/4");
                reported_absence = false;
            }
        }
        {
            std::lock_guard lock(mutex);
            ready = true;
            public_active.store(initialized);
        }
        ready_cv.notify_all();

        std::vector<Voice> voices;
        uint64_t rendered_frames{};
        while (initialized && !stop.load()) {
            WaitForSingleObject(event_handle, 100);
            UINT32 buffer_frames{}, padding{};
            if (FAILED(client->GetBufferSize(&buffer_frames)) ||
                FAILED(client->GetCurrentPadding(&padding))) {
                log_line("Haptics: controller audio stream lost; waiting for USB reconnect");
                break;
            }
            const UINT32 available = buffer_frames - padding;
            if (!available) continue;
            {
                std::lock_guard lock(mutex);
                for (auto& voice : pending) {
                    voice.frame = rendered_frames;
                    voices.push_back(voice);
                }
                pending.clear();
            }
            BYTE* buffer{};
            if (FAILED(renderer->GetBuffer(available, &buffer))) continue;
            memset(buffer, 0, static_cast<size_t>(available) * format->nBlockAlign);
            for (UINT32 f = 0; f < available; ++f) {
                float left{}, right{};
                for (const auto& voice : voices) {
                    const double t = static_cast<double>(rendered_frames + f - voice.frame) /
                                     format->nSamplesPerSec;
                    synthesize(voice, t, left, right);
                }
                BYTE* frame = buffer + static_cast<size_t>(f) * format->nBlockAlign;
                write_sample(frame, format, 2, left);
                write_sample(frame, format, 3, right);
            }
            renderer->ReleaseBuffer(available, 0);
            rendered_frames += available;
            std::erase_if(voices, [&](const Voice& voice) {
                return static_cast<double>(rendered_frames - voice.frame) / format->nSamplesPerSec >=
                       duration(voice.effect);
            });
        }
        public_active.store(false);
        if (client && initialized) client->Stop();
        release(renderer);
        release(client);
        release(selected);
        if (format) CoTaskMemFree(format);
        if (event_handle) CloseHandle(event_handle);
        CoUninitialize();
    }

    void audio_loop(std::atomic_bool& public_active) {
        do {
            audio_session(public_active);
            for (unsigned waited = 0; waited < 50 && !stop.load(); ++waited) Sleep(100);
        } while (!stop.load());
    }
};

Haptics::Haptics() : impl_(std::make_unique<Impl>()) {}
Haptics::~Haptics() { stop(); }

bool Haptics::start() {
    impl_->worker = std::thread([this] { impl_->audio_loop(active_); });
    std::unique_lock lock(impl_->mutex);
    impl_->ready_cv.wait_for(lock, std::chrono::seconds(3), [this] { return impl_->ready; });
    return active();
}

void Haptics::stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
}

void Haptics::play(HapticEffect effect, float strength) {
    if (!active() || strength <= 0.0f) return;
    std::lock_guard lock(impl_->mutex);
    impl_->pending.push_back({effect, std::clamp(strength, 0.0f, 1.0f), 0});
}
