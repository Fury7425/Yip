#pragma once

// Thin C++ wrappers over the audio-core C ABI: RAII for device lists, COM
// `IMMNotificationClient` watcher that triggers a callback on the UI thread
// when endpoints change.

#include <functional>
#include <string>
#include <vector>

#include "audio_core.h"

namespace yip::interop
{
    struct Device
    {
        std::string id;     // utf-8
        std::string name;   // utf-8
        bool isCapture{ true };
        bool isDefault{ false };
    };

    // Enumerate endpoints. On error returns an empty vector and silently
    // swallows — callers can use `rec_last_error()` to read the message.
    std::vector<Device> ListDevices();

    // RAII watcher: registers an `IMMNotificationClient` against the system
    // device enumerator. `onChanged` is invoked from a WASAPI worker thread
    // — callers must marshal to the UI dispatcher themselves.
    class DeviceWatcher
    {
    public:
        explicit DeviceWatcher(std::function<void()> onChanged);
        ~DeviceWatcher();
        DeviceWatcher(const DeviceWatcher&)            = delete;
        DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
