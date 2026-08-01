// waveform_player.exe — plays NHW1 haptic codes on the DualSense actuators.
//
// Reads commands from stdin, one per line:
//     play <code>          play the waveform once
//     play <code> <count>  play it <count> times, 250 ms apart
//     quit
//
// Writes one line per command to stdout so a front end can follow along.
//
// The NHW1 code is the interchange format between this studio and the mod:
//
//   NHW1;dur=0.130;gain=0.62;bal=0.30@23;L=90,0.55,55;L=430,0.17,11,23,0.4
//
//   dur   total length in seconds
//   gain  overall intensity, 0..1
//   bal   stereo balance -1..1; "@hz" sweeps it between the actuators
//   L     a layer: frequency, amplitude, decay, then optional tremolo hz and depth
//
// Each layer is amplitude * exp(-decay * t) * sin(2*pi*frequency*t), optionally
// multiplied by (1 + depth * sin(2*pi*tremolo*t)).
#include <Windows.h>
#include <audioclient.h>
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Layer {
    double frequency{};
    double amplitude{};
    double decay{};
    double tremolo_hz{};
    double tremolo_depth{};
};

struct Waveform {
    double duration{0.1};
    double gain{0.5};
    double balance{};
    double balance_hz{};
    std::vector<Layer> layers;
};

template <typename T> void release(T*& value) {
    if (value) value->Release();
    value = nullptr;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, at == std::string::npos ? at : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return parts;
}

std::string trim(std::string text) {
    while (!text.empty() && isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

bool parse(const std::string& code, Waveform& out, std::string& error) {
    Waveform result;
    bool saw_header = false;
    for (const auto& raw : split(code, ';')) {
        const std::string field = trim(raw);
        if (field.empty()) continue;
        if (_strnicmp(field.c_str(), "NHW1", 4) == 0) { saw_header = true; continue; }
        const size_t equals = field.find('=');
        if (equals == std::string::npos) { error = "field without '=': " + field; return false; }
        const std::string key = trim(field.substr(0, equals));
        const std::string value = trim(field.substr(equals + 1));
        if (_stricmp(key.c_str(), "dur") == 0) {
            result.duration = atof(value.c_str());
        } else if (_stricmp(key.c_str(), "gain") == 0) {
            result.gain = atof(value.c_str());
        } else if (_stricmp(key.c_str(), "bal") == 0) {
            const size_t at = value.find('@');
            result.balance = atof(value.substr(0, at).c_str());
            if (at != std::string::npos) result.balance_hz = atof(value.substr(at + 1).c_str());
        } else if (_stricmp(key.c_str(), "l") == 0) {
            const auto numbers = split(value, ',');
            if (numbers.size() < 3) { error = "layer needs frequency, amplitude, decay"; return false; }
            Layer layer;
            layer.frequency = atof(numbers[0].c_str());
            layer.amplitude = atof(numbers[1].c_str());
            layer.decay = atof(numbers[2].c_str());
            if (numbers.size() > 3) layer.tremolo_hz = atof(numbers[3].c_str());
            if (numbers.size() > 4) layer.tremolo_depth = atof(numbers[4].c_str());
            result.layers.push_back(layer);
        } else {
            error = "unknown field: " + key;
            return false;
        }
    }
    if (!saw_header) { error = "code must start with NHW1"; return false; }
    if (result.layers.empty()) { error = "code has no layers"; return false; }
    result.duration = std::clamp(result.duration, 0.005, 3.0);
    result.gain = std::clamp(result.gain, 0.0, 1.0);
    result.balance = std::clamp(result.balance, -1.0, 1.0);
    out = result;
    return true;
}

void sample_at(const Waveform& wave, double t, float& left, float& right) {
    double value = 0.0;
    for (const auto& layer : wave.layers) {
        double amplitude = layer.amplitude * std::exp(-layer.decay * t);
        if (layer.tremolo_hz > 0.0)
            amplitude *= 1.0 + layer.tremolo_depth * std::sin(2.0 * kPi * layer.tremolo_hz * t);
        value += amplitude * std::sin(2.0 * kPi * layer.frequency * t);
    }
    value *= wave.gain;
    const double balance = wave.balance_hz > 0.0
        ? wave.balance * std::sin(2.0 * kPi * wave.balance_hz * t)
        : wave.balance;
    left = static_cast<float>(value * (balance > 0.0 ? 1.0 - balance : 1.0));
    right = static_cast<float>(value * (balance < 0.0 ? 1.0 + balance : 1.0));
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
    if (is_float && bytes == 4) *reinterpret_cast<float*>(target) = value;
    else if (bytes == 2) *reinterpret_cast<int16_t*>(target) = static_cast<int16_t>(value * 32767.0f);
    else if (bytes == 3) {
        const int32_t v = static_cast<int32_t>(value * 8388607.0f);
        target[0] = static_cast<BYTE>(v);
        target[1] = static_cast<BYTE>(v >> 8);
        target[2] = static_cast<BYTE>(v >> 16);
    } else if (bytes == 4) {
        *reinterpret_cast<int32_t*>(target) = static_cast<int32_t>(value * 2147483647.0f);
    }
}

struct Output {
    IMMDevice* device{};
    IAudioClient* client{};
    IAudioRenderClient* renderer{};
    WAVEFORMATEX* format{};
    HANDLE event{};

    bool open(std::string& error) {
        IMMDeviceEnumerator* enumerator{};
        IMMDeviceCollection* devices{};
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
        UINT count{};
        if (SUCCEEDED(hr)) devices->GetCount(&count);
        for (UINT i = 0; i < count && !device; ++i) {
            IMMDevice* candidate{};
            IPropertyStore* properties{};
            PROPVARIANT name;
            PropVariantInit(&name);
            devices->Item(i, &candidate);
            candidate->OpenPropertyStore(STGM_READ, &properties);
            if (properties) properties->GetValue(PKEY_Device_FriendlyName, &name);
            const std::wstring friendly = name.vt == VT_LPWSTR ? name.pwszVal : L"";
            const bool dualsense = friendly.find(L"DualSense") != std::wstring::npos ||
                                   friendly.find(L"Wireless Controller") != std::wstring::npos;
            IAudioClient* candidate_client{};
            WAVEFORMATEX* candidate_format{};
            if (dualsense &&
                SUCCEEDED(candidate->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(&candidate_client))) &&
                SUCCEEDED(candidate_client->GetMixFormat(&candidate_format)) &&
                candidate_format->nChannels >= 4) {
                device = candidate;
                device->AddRef();
                client = candidate_client;
                candidate_client = nullptr;
                format = candidate_format;
                candidate_format = nullptr;
                wprintf(L"endpoint '%ls' %u Hz %u ch\n", friendly.c_str(),
                        format->nSamplesPerSec, format->nChannels);
            }
            if (candidate_format) CoTaskMemFree(candidate_format);
            release(candidate_client);
            PropVariantClear(&name);
            release(properties);
            release(candidate);
        }
        release(devices);
        release(enumerator);
        if (!device) { error = "no active four-channel DualSense endpoint found"; return false; }

        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0, format, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient),
                                                   reinterpret_cast<void**>(&renderer));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) { error = "audio initialisation failed"; return false; }
        return true;
    }

    void play(const Waveform& wave) {
        const auto rate = static_cast<double>(format->nSamplesPerSec);
        const auto total = static_cast<UINT64>(wave.duration * rate);
        UINT64 written = 0;
        while (written < total) {
            WaitForSingleObject(event, 200);
            UINT32 buffer_frames{}, padding{};
            if (FAILED(client->GetBufferSize(&buffer_frames)) ||
                FAILED(client->GetCurrentPadding(&padding))) return;
            UINT32 available = buffer_frames - padding;
            if (!available) continue;
            available = static_cast<UINT32>(std::min<UINT64>(available, total - written));
            BYTE* buffer{};
            if (FAILED(renderer->GetBuffer(available, &buffer))) return;
            memset(buffer, 0, static_cast<size_t>(available) * format->nBlockAlign);
            for (UINT32 f = 0; f < available; ++f) {
                float left{}, right{};
                sample_at(wave, static_cast<double>(written + f) / rate, left, right);
                BYTE* frame = buffer + static_cast<size_t>(f) * format->nBlockAlign;
                write_sample(frame, format, 2, left);
                write_sample(frame, format, 3, right);
            }
            renderer->ReleaseBuffer(available, 0);
            written += available;
        }
    }

    void close() {
        if (client) client->Stop();
        release(renderer);
        release(client);
        release(device);
        if (format) CoTaskMemFree(format);
        if (event) CloseHandle(event);
    }
};
}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Output output;
    std::string error;
    if (!output.open(error)) {
        printf("error %s\n", error.c_str());
        return 1;
    }
    printf("ready\n");

    char line[2048];
    while (fgets(line, sizeof(line), stdin)) {
        std::string command = trim(line);
        if (command.empty()) continue;
        if (_stricmp(command.c_str(), "quit") == 0) break;
        if (_strnicmp(command.c_str(), "play", 4) != 0) {
            printf("error unknown command\n");
            continue;
        }
        std::string rest = trim(command.substr(4));
        int repeats = 1;
        const size_t space = rest.rfind(' ');
        if (space != std::string::npos) {
            const std::string tail = trim(rest.substr(space + 1));
            if (!tail.empty() && tail.find_first_not_of("0123456789") == std::string::npos) {
                repeats = std::clamp(atoi(tail.c_str()), 1, 32);
                rest = trim(rest.substr(0, space));
            }
        }
        Waveform wave;
        if (!parse(rest, wave, error)) {
            printf("error %s\n", error.c_str());
            continue;
        }
        for (int i = 0; i < repeats; ++i) {
            output.play(wave);
            if (i + 1 < repeats) Sleep(250);
        }
        printf("played %.0f ms, %llu layer(s), %d time(s)\n", wave.duration * 1000.0,
               static_cast<unsigned long long>(wave.layers.size()), repeats);
    }
    output.close();
    CoUninitialize();
    return 0;
}
