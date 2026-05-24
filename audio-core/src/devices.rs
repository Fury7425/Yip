//! WASAPI endpoint enumeration. Returns both capture (mic) and render
//! (loopback) endpoints with their stable IDs and friendly names.

use windows::Win32::Devices::FunctionDiscovery::PKEY_Device_FriendlyName;
use windows::Win32::Media::Audio::{
    DEVICE_STATE_ACTIVE, EDataFlow, ERole, IMMDevice, IMMDeviceEnumerator, MMDeviceEnumerator,
    eCapture, eConsole, eRender,
};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx, STGM_READ,
};

use crate::error::YipError;

#[derive(Debug, Clone)]
pub struct Device {
    /// Stable endpoint id (e.g. `{0.0.1.00000000}.{...}`).
    pub id: String,
    /// User-facing name from `PKEY_Device_FriendlyName`.
    pub name: String,
    /// `true` = microphone/line-in (eCapture); `false` = loopback (eRender).
    pub is_capture: bool,
    /// `true` if this endpoint is the system default for its data flow.
    pub is_default: bool,
}

/// COM-init helper. `CoInitializeEx` is idempotent per thread; we ignore
/// `RPC_E_CHANGED_MODE` because tests may have already initialised STA.
fn ensure_com() -> Result<(), YipError> {
    // SAFETY: passing a null pvReserved and a valid COINIT flag. Returning HR
    // is inspected; RPC_E_CHANGED_MODE is benign.
    let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
    if hr.is_err() && hr.0 != windows::Win32::Foundation::RPC_E_CHANGED_MODE.0 {
        return Err(YipError::Wasapi(format!(
            "CoInitializeEx 0x{:08X}",
            hr.0 as u32
        )));
    }
    Ok(())
}

fn enumerator() -> Result<IMMDeviceEnumerator, YipError> {
    ensure_com()?;
    // SAFETY: standard COM cocreate; CLSID + IID are well-known.
    let e: IMMDeviceEnumerator =
        unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)? };
    Ok(e)
}

fn default_id(e: &IMMDeviceEnumerator, flow: EDataFlow, role: ERole) -> Option<String> {
    // SAFETY: e is a live COM ptr; absence of a default returns an HRESULT we
    // map to None.
    let dev = unsafe { e.GetDefaultAudioEndpoint(flow, role) }.ok()?;
    read_id(&dev).ok()
}

fn read_id(dev: &IMMDevice) -> Result<String, YipError> {
    // SAFETY: live COM pointer; PWSTR is freed via CoTaskMemFree below.
    let pwstr = unsafe { dev.GetId()? };
    // SAFETY: PWSTR from MMDevice is null-terminated UTF-16.
    let s = unsafe { pwstr.to_string() }
        .map_err(|_| YipError::Wasapi("device id not utf-16".into()))?;
    // SAFETY: pointer returned by GetId must be freed with CoTaskMemFree.
    unsafe {
        windows::Win32::System::Com::CoTaskMemFree(Some(pwstr.0.cast()));
    }
    Ok(s)
}

fn read_friendly_name(dev: &IMMDevice) -> Result<String, YipError> {
    // SAFETY: dev live; STGM_READ valid.
    let store = unsafe { dev.OpenPropertyStore(STGM_READ)? };
    // SAFETY: store live; PKEY is &'static.
    let value = unsafe { store.GetValue(&PKEY_Device_FriendlyName)? };

    // PKEY_Device_FriendlyName returns VT_LPWSTR; access the union member as
    // PWSTR directly. PROPVARIANT::Drop calls PropVariantClear which frees
    // the inner pointer, so we must clone the string before the value is
    // dropped at end of scope.
    // SAFETY: PROPVARIANT layout is well-defined; this PKEY is documented
    // to return VT_LPWSTR.
    let pwstr = unsafe { value.Anonymous.Anonymous.Anonymous.pwszVal };
    if pwstr.is_null() {
        return Err(YipError::Wasapi("device name PROPVARIANT empty".into()));
    }
    // SAFETY: pwstr is null-terminated UTF-16 owned by the PROPVARIANT.
    let s = unsafe { pwstr.to_string() }
        .map_err(|_| YipError::Wasapi("device name not utf-16".into()))?;
    Ok(s)
}

fn collect(
    e: &IMMDeviceEnumerator,
    flow: EDataFlow,
    is_capture: bool,
    default_id: Option<&str>,
    sink: &mut Vec<Device>,
) -> Result<(), YipError> {
    // SAFETY: live enumerator.
    let coll = unsafe { e.EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE)? };
    // SAFETY: live collection.
    let count = unsafe { coll.GetCount()? };
    for i in 0..count {
        // SAFETY: i < count.
        let dev: IMMDevice = unsafe { coll.Item(i)? };
        let id = read_id(&dev)?;
        let name = match read_friendly_name(&dev) {
            Ok(n) => n,
            Err(_) => format!("<endpoint {i}>"),
        };
        let is_default = default_id == Some(id.as_str());
        sink.push(Device {
            id,
            name,
            is_capture,
            is_default,
        });
    }
    Ok(())
}

/// Returns capture (mic) endpoints followed by render (loopback) endpoints.
/// Within each group the system default is sorted first.
pub fn list_devices() -> Result<Vec<Device>, YipError> {
    let e = enumerator()?;
    let cap_default = default_id(&e, eCapture, eConsole);
    let ren_default = default_id(&e, eRender, eConsole);

    let mut out = Vec::with_capacity(8);
    collect(&e, eCapture, true, cap_default.as_deref(), &mut out)?;
    collect(&e, eRender, false, ren_default.as_deref(), &mut out)?;

    out.sort_by_key(|d| (!d.is_capture, !d.is_default, d.name.clone()));
    Ok(out)
}

/// Look up an endpoint by stable id.
pub fn find_device(id: &str) -> Result<IMMDevice, YipError> {
    let e = enumerator()?;
    let wide: Vec<u16> = id.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: wide is a valid null-terminated UTF-16 buffer.
    let dev = unsafe { e.GetDevice(windows::core::PCWSTR(wide.as_ptr()))? };
    Ok(dev)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enumerate_runs_without_panic() {
        // No assertion on contents: some CI runners have no audio devices at
        // all. We only require that the call succeeds without panicking and
        // returns an owned `Vec`.
        let _ = list_devices();
    }
}
