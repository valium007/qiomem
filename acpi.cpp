#include <stdio.h>
#include <swdevice.h>
#include <windows.h>

#pragma comment(lib, "swdevice.lib")

#define TARGET_HWID L"ACPI\\QCI0701"

static HANDLE g_hCreateDone = nullptr;

void WINAPI DeviceCreateCallback(
    HSWDEVICE hSwDevice,
    HRESULT hrCreateResult,
    PVOID pContext,
    PCWSTR pszDeviceInstanceId)
{
    if (SUCCEEDED(hrCreateResult))
        printf("[+] Virtual device created: %ws\n", pszDeviceInstanceId);
    else
        printf("[-] Device creation failed: 0x%08X\n", hrCreateResult);

    SetEvent(g_hCreateDone);
}

int main()
{
    g_hCreateDone = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    const wchar_t hwIds[] = TARGET_HWID L"\0";

    SW_DEVICE_CREATE_INFO createInfo {};
    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszInstanceId = L"FakeACPIDevice0001";
    createInfo.pszzHardwareIds = hwIds;
    createInfo.CapabilityFlags = SWDeviceCapabilitiesNone;
    createInfo.pszDeviceDescription = L"Virtual ACPI Device";

    HSWDEVICE hSwDevice = nullptr;

    HRESULT hr = SwDeviceCreate(
        L"MyVirtualBus",
        L"HTREE\\ROOT\\0",
        &createInfo,
        0,
        nullptr,
        DeviceCreateCallback,
        nullptr,
        &hSwDevice);

    if (FAILED(hr)) {
        printf("[-] SwDeviceCreate failed: 0x%08X\n", hr);
        return 1;
    }

    WaitForSingleObject(g_hCreateDone, 10000);

    printf("[*] Device node alive — PnP should now load the driver.\n");
    printf("[*] Press Enter to remove the device and exit...\n");
    getchar();

    SwDeviceClose(hSwDevice);
    CloseHandle(g_hCreateDone);
    return 0;
}