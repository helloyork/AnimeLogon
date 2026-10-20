#include "devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace devices {

std::vector<Output> ListOutputs() {
    std::vector<Output> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        Output o;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            o.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                o.name = v.pwszVal;
            PropVariantClear(&v);
        }
        if (!o.id.empty()) out.push_back(std::move(o));
    }
    return out;
}

}  // namespace devices
