#include "pch.h"
#include "AudioCoreInterop.h"

#include <atomic>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace yip::interop
{
    std::vector<Device> ListDevices()
    {
        std::vector<Device> out;
        DeviceInfo* arr = nullptr;
        size_t len = 0;
        const auto status = rec_list_devices(&arr, &len);
        if (status != REC_STATUS_OK || !arr) {
            return out;
        }
        out.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            Device d;
            d.id        = arr[i].id   ? arr[i].id   : "";
            d.name      = arr[i].name ? arr[i].name : "";
            d.isCapture = arr[i].kind != 0;
            d.isDefault = arr[i].is_default != 0;
            out.push_back(std::move(d));
        }
        rec_free_devices(arr, len);
        return out;
    }

    // ---- DeviceWatcher implementation ----

    namespace {

    class NotificationClient
        : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMNotificationClient>
    {
    public:
        explicit NotificationClient(std::function<void()> cb) : m_cb(std::move(cb)) {}

        IFACEMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override        { fire(); return S_OK; }
        IFACEMETHODIMP OnDeviceAdded(LPCWSTR) override                      { fire(); return S_OK; }
        IFACEMETHODIMP OnDeviceRemoved(LPCWSTR) override                    { fire(); return S_OK; }
        IFACEMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { fire(); return S_OK; }
        IFACEMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    private:
        void fire() {
            // Debounce: only forward if a callback isn't already in flight.
            if (!m_inflight.exchange(true)) {
                if (m_cb) m_cb();
                m_inflight.store(false);
            }
        }

        std::function<void()> m_cb;
        std::atomic<bool> m_inflight{ false };
    };

    } // namespace

    struct DeviceWatcher::Impl
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<NotificationClient>  client;
    };

    DeviceWatcher::DeviceWatcher(std::function<void()> onChanged)
        : m_impl(std::make_unique<Impl>())
    {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            return;
        }
        hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(m_impl->enumerator.GetAddressOf()));
        if (FAILED(hr)) return;
        m_impl->client = Microsoft::WRL::Make<NotificationClient>(std::move(onChanged));
        if (!m_impl->client) return;
        m_impl->enumerator->RegisterEndpointNotificationCallback(m_impl->client.Get());
    }

    DeviceWatcher::~DeviceWatcher()
    {
        if (m_impl && m_impl->enumerator && m_impl->client) {
            m_impl->enumerator->UnregisterEndpointNotificationCallback(m_impl->client.Get());
        }
    }
}
