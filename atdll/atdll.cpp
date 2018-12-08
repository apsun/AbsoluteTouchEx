#include <exception>
#include <memory>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidusage.h>
#include "detours.h"

#define DEBUG_MODE 1
#define HID_USAGE_DIGITIZER_CONTACT_ID 0x51
#define HID_USAGE_DIGITIZER_CONTACT_COUNT 0x54
#define MAGIC_HANDLE ((HRAWINPUT)0)

// C++ exception wrapping the Win32 GetLastError() status
class win32_error : std::exception
{
public:
    win32_error(DWORD errorCode)
        : m_errorCode(errorCode)
    {

    }

    win32_error()
        : win32_error(GetLastError())
    {

    }

    DWORD code() const
    {
        return m_errorCode;
    }

private:
    DWORD m_errorCode;
};

// C++ exception wrapping the HIDP_STATUS_* codes
class hid_error : std::exception
{
public:
    hid_error(NTSTATUS status)
        : m_errorCode(status)
    {

    }

    NTSTATUS code() const
    {
        return m_errorCode;
    }

private:
    NTSTATUS m_errorCode;
};

// Wrapper for malloc with unique_ptr semantics, to allow
// for variable-sized structures.
struct free_deleter { void operator()(void *ptr) { free(ptr); } };
template<typename T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

// Contact information parsed from the HID report descriptor.
struct at_contact_info
{
    USHORT link;
    RECT touchArea;
};

// The data for a touch event.
struct at_contact
{
    at_contact_info info;
    ULONG id;
    POINT point;
};

// Device information, such as touch area bounds and HID offsets.
// This can be reused across HID events, so we only have to parse
// this info once.
struct at_device_info
{
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData; // HID internal data
    USHORT linkContactCount; // Link collection for number of contacts present
    std::vector<at_contact_info> contactInfo; // Link collection and touch area for each contact
};

// Hook trampolines
static decltype(RegisterRawInputDevices) *g_originalRegisterRawInputDevices = RegisterRawInputDevices;
static decltype(CreateWindowExW) *g_originalCreateWindowExW = CreateWindowExW;
static decltype(GetRawInputData) *g_originalGetRawInputData = GetRawInputData;
static decltype(RegisterClassExW) *g_originalRegisterClassExW = RegisterClassExW;
#if _WIN64
static decltype(GetWindowLongPtrW) *g_originalGetWindowLongPtrW = GetWindowLongPtrW;
static decltype(SetWindowLongPtrW) *g_originalSetWindowLongPtrW = SetWindowLongPtrW;
#else
static decltype(GetWindowLongW) *g_originalGetWindowLongW = GetWindowLongW;
static decltype(SetWindowLongW) *g_originalSetWindowLongW = SetWindowLongW;
#endif
static std::unordered_map<HWND, WNDPROC> g_originalWndProcs;

// Caches per-device info for better performance
static std::unordered_map<HANDLE, at_device_info> g_devices;

// Holds the injected mouse input to be consumed by the real WndProc()
static thread_local RAWINPUT t_injectedInput;

// Holds the current primary touch point ID
static thread_local ULONG t_primaryContactID;

// Allocates a malloc_ptr with the given size. The size must be
// greater than or equal to sizeof(T).
template<typename T>
static malloc_ptr<T>
make_malloc(size_t size)
{
    T *ptr = (T *)malloc(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return malloc_ptr<T>(ptr);
}

// C-style printf for debug output.
static void
debugf(const char *fmt, ...)
{
#if DEBUG_MODE
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
#endif
}

// Reads the raw input header for the given raw input handle.
static RAWINPUTHEADER
AT_GetRawInputHeader(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr;
    UINT size = sizeof(hdr);
    if (g_originalGetRawInputData(hInput, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw win32_error();
    }
    return hdr;
}

// Reads the raw input data for the given raw input handle.
static malloc_ptr<RAWINPUT>
AT_GetRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    malloc_ptr<RAWINPUT> input = make_malloc<RAWINPUT>(hdr.dwSize);
    UINT size = hdr.dwSize;
    if (g_originalGetRawInputData(hInput, RID_INPUT, input.get(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw win32_error();
    }
    return input;
}

// Reads the preparsed HID report descriptor for the device
// that generated the given raw input.
static malloc_ptr<_HIDP_PREPARSED_DATA>
AT_GetHidPreparsedData(RAWINPUTHEADER hdr)
{
    UINT size;
    if (GetRawInputDeviceInfoW(hdr.hDevice, RIDI_PREPARSEDDATA, nullptr, &size) == (UINT)-1) {
        throw win32_error();
    }
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData = make_malloc<_HIDP_PREPARSED_DATA>(size);
    if (GetRawInputDeviceInfoW(hdr.hDevice, RIDI_PREPARSEDDATA, preparsedData.get(), &size) == (UINT)-1) {
        throw win32_error();
    }
    return preparsedData;
}

// Returns all input button caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_BUTTON_CAPS>
AT_GetHidInputButtonCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    USHORT numCaps = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttonCaps(numCaps);
    status = HidP_GetButtonCaps(HidP_Input, &buttonCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    buttonCaps.resize(numCaps);
    return buttonCaps;
}

// Returns all input value caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_VALUE_CAPS>
AT_GetHidInputValueCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    USHORT numCaps = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(numCaps);
    status = HidP_GetValueCaps(HidP_Input, &valueCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    valueCaps.resize(numCaps);
    return valueCaps;
}

// Reads the pressed status of a single HID report button.
static bool
AT_GetHidUsageButton(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG numUsages = HidP_MaxUsageListLength(
        reportType,
        usagePage,
        preparsedData);
    std::vector<USAGE> usages(numUsages);
    NTSTATUS status = HidP_GetUsages(
        reportType,
        usagePage,
        linkCollection,
        &usages[0],
        &numUsages,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    usages.resize(numUsages);
    return std::find(usages.begin(), usages.end(), usage) != usages.end();
}

// Reads a single HID report value in logical units.
static ULONG
AT_GetHidUsageLogicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG value;
    NTSTATUS status = HidP_GetUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    return value;
}

// Reads a single HID report value in physical units.
static LONG
AT_GetHidUsagePhysicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    LONG value;
    NTSTATUS status = HidP_GetScaledUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    return value;
}

// Registers the specified window to receive touchpad HID events.
static void
AT_RegisterTouchpadInput(HWND hWnd)
{
    RAWINPUTDEVICE dev;
    dev.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    dev.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hWnd;
    if (!g_originalRegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE))) {
        throw win32_error();
    }
}

// Converts a touchpad point to a "screen" point. Note that a screen
// point here is not a actual pixel coordinate, but a value between 0
// and 65535. (0, 0) is the top-left corner, (65535, 65535) is the
// bottom-right corner.
static POINT
AT_TouchpadToScreen(RECT touchpadRect, POINT touchpadPoint)
{
    int tpDeltaX = touchpadPoint.x - touchpadRect.left;
    int tpDeltaY = touchpadPoint.y - touchpadRect.top;

    int scDeltaX = tpDeltaX * 65535 / (touchpadRect.right - touchpadRect.left);
    int scDeltaY = tpDeltaY * 65535 / (touchpadRect.bottom - touchpadRect.top);

    POINT screenPoint;
    screenPoint.x = scDeltaX;
    screenPoint.y = scDeltaY;

    return screenPoint;
}

// Gets the device info associated with the given raw input. Uses the
// cached info if available; otherwise parses the HID report descriptor
// and stores it into the cache.
static at_device_info &
AT_GetDeviceInfo(RAWINPUTHEADER hdr)
{
    if (g_devices.count(hdr.hDevice)) {
        return g_devices[hdr.hDevice];
    }
    
    at_device_info dev;
    dev.preparsedData = AT_GetHidPreparsedData(hdr);

    // Struct to hold our parser state
    struct at_contact_info_tmp
    {
        bool hasContactID = false;
        bool hasTip = false;
        bool hasX = false;
        bool hasY = false;
        RECT touchArea;
    };
    std::unordered_map<USHORT, at_contact_info_tmp> contacts;

    // Get the touch area for all the contacts. Also make sure that each one
    // is actually a contact, as specified by:
    // https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections
    for (const HIDP_VALUE_CAPS &cap : AT_GetHidInputValueCaps(dev.preparsedData.get())) {
        if (cap.IsRange || !cap.IsAbsolute) {
            continue;
        }

        if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
                dev.linkContactCount = cap.LinkCollection;
            }
        }

        if (cap.UsagePage == HID_USAGE_PAGE_GENERIC) {
            if (cap.NotRange.Usage == HID_USAGE_GENERIC_X) {
                contacts[cap.LinkCollection].touchArea.left = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.right = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasX = true;
            } else if (cap.NotRange.Usage == HID_USAGE_GENERIC_Y) {
                contacts[cap.LinkCollection].touchArea.top = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.bottom = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasY = true;
            }
        } else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_ID) {
                contacts[cap.LinkCollection].hasContactID = true;
            }
        }
    }

    for (const HIDP_BUTTON_CAPS &cap : AT_GetHidInputButtonCaps(dev.preparsedData.get())) {
        if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_TIP_SWITCH) {
                contacts[cap.LinkCollection].hasTip = true;
            }
        }
    }

    for (auto &kvp : contacts) {
        USHORT link = kvp.first;
        at_contact_info_tmp &info = kvp.second;
        if (info.hasContactID && info.hasTip && info.hasX && info.hasY) {
            debugf("Contact for device %p: link=%d, touchArea={%d,%d,%d,%d}\n",
                hdr.hDevice,
                link,
                info.touchArea.left,
                info.touchArea.top,
                info.touchArea.right,
                info.touchArea.bottom);
            dev.contactInfo.push_back({ link, info.touchArea });
        }
    }

    return g_devices[hdr.hDevice] = std::move(dev);
}

// Reads all touch contact points from a raw input event.
static std::vector<at_contact>
AT_GetContacts(at_device_info &dev, RAWINPUT *input)
{
    std::vector<at_contact> contacts;

    DWORD sizeHid = input->data.hid.dwSizeHid;
    DWORD count = input->data.hid.dwCount;
    BYTE *rawData = input->data.hid.bRawData;
    if (count == 0) {
        return contacts;
    }

    ULONG numContacts = AT_GetHidUsageLogicalValue(
        HidP_Input,
        HID_USAGE_PAGE_DIGITIZER,
        dev.linkContactCount,
        HID_USAGE_DIGITIZER_CONTACT_COUNT,
        dev.preparsedData.get(),
        rawData,
        sizeHid);
    
    if (numContacts > dev.contactInfo.size()) {
        throw std::runtime_error("Device reported more contacts than we have links");
    }

    // It's a little ambiguous as to whether contact count includes
    // released contacts. I interpreted the specs as a yes, but this
    // may require additional testing.
    for (ULONG i = 0; i < numContacts; ++i) {
        at_contact_info &info = dev.contactInfo[i];
        bool tip = AT_GetHidUsageButton(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_TIP_SWITCH,
            dev.preparsedData.get(),
            rawData,
            sizeHid);
        
        if (!tip) {
            continue;
        }

        ULONG id = AT_GetHidUsageLogicalValue(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_CONTACT_ID,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG x = AT_GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_X,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG y = AT_GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_Y,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        contacts.push_back({ info, id, { x, y } });
    }

    return contacts;
}

// Returns the primary contact for a given list of contacts. This is
// necessary since we are mapping potentially many touches to a single
// mouse position. Currently this just stores a global contact ID and
// uses that as the primary contact.
static at_contact
AT_GetPrimaryContact(const std::vector<at_contact> &contacts)
{
    for (at_contact contact : contacts) {
        if (contact.id == t_primaryContactID) {
            return contact;
        }
    }
    t_primaryContactID = contacts[0].id;
    return contacts[0];
}

// Handles a WM_INPUT event. May update wParam/lParam to be delivered
// to the real WndProc. Returns true if the event is handled entirely
// at the hook layer and should not be delivered to the real WndProc.
// Returns false if the real WndProc should be called.
static bool
AT_HandleRawInput(WPARAM *wParam, LPARAM *lParam)
{
    HRAWINPUT hInput = (HRAWINPUT)*lParam;
    RAWINPUTHEADER hdr = AT_GetRawInputHeader(hInput);
    if (hdr.dwType != RIM_TYPEHID) {
        // Suppress mouse input events to prevent it from getting
        // mixed in with our absolute movement events. Unfortunately
        // this has the side effect of disabling all non-touchpad
        // input. One solution might be to determine the device that
        // sent the event and check if it's also a touchpad, and only
        // filter out events from such devices.
        if (hdr.dwType == RIM_TYPEMOUSE) {
            return true;
        }
        return false;
    }
    
    at_device_info &dev = AT_GetDeviceInfo(hdr);
    malloc_ptr<RAWINPUT> input = AT_GetRawInput(hInput, hdr);
    std::vector<at_contact> contacts = AT_GetContacts(dev, input.get());
    if (contacts.size() == 0) {
        return true;
    }

    at_contact contact = AT_GetPrimaryContact(contacts);
    POINT screenPoint = AT_TouchpadToScreen(contact.info.touchArea, contact.point);

    t_injectedInput.header.dwType = RIM_TYPEMOUSE;
    t_injectedInput.header.dwSize = sizeof(RAWINPUT);
    t_injectedInput.header.wParam = *wParam;
    t_injectedInput.header.hDevice = input->header.hDevice;
    t_injectedInput.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
    t_injectedInput.data.mouse.ulExtraInformation = 0;
    t_injectedInput.data.mouse.usButtonFlags = 0;
    t_injectedInput.data.mouse.usButtonData = 0;
    t_injectedInput.data.mouse.lLastX = screenPoint.x;
    t_injectedInput.data.mouse.lLastY = screenPoint.y;

    *lParam = (LPARAM)MAGIC_HANDLE;
    debugf("AT_HandleRawInput: swizzled handle\n");
    return false;
}

// Hook for GetRawInputData(). Copies the raw input data given a
// handle that was generated from AT_HandleRawInput(). If the handle is
// not from AT_HandleRawInput(), delegates to the real GetRawInputData().
static UINT WINAPI
AT_GetRawInputDataHook(
    HRAWINPUT hRawInput,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize,
    UINT cbSizeHeader)
{
    debugf("GetRawInputData(handle=%p, command=0x%x)\n", hRawInput, uiCommand);

    if (hRawInput != MAGIC_HANDLE) {
        return g_originalGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    }

    if (cbSizeHeader != sizeof(RAWINPUTHEADER)) {
        return (UINT)-1;
    }

    PVOID data;
    UINT size;
    switch (uiCommand) {
    case RID_HEADER:
        data = &t_injectedInput.header;
        size = sizeof(t_injectedInput.header);
        break;
    case RID_INPUT:
        data = &t_injectedInput;
        size = sizeof(t_injectedInput);
        break;
    default:
        return (UINT)-1;
    }
    
    if (pData == nullptr) {
        *pcbSize = size;
        return 0;
    } else if (*pcbSize < size) {
        return (UINT)-1;
    } else {
        memcpy(pData, data, size);
        *pcbSize = size;
        return size;
    }
}

// Our fake WndProc that intercepts any WM_INPUT messages.
// Any non-WM_INPUT messages and unhandled WM_INPUT messages
// are delivered to the real WndProc.
static LRESULT CALLBACK
AT_WndProcHook(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    // Intercept and discard normal mouse movement messages
    if (message == WM_MOUSEMOVE) {
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    if (message == WM_INPUT) {
        bool handled = false;
        try {
            handled = AT_HandleRawInput(&wParam, &lParam);
        } catch (const win32_error &e) {
            debugf("WndProc: win32_error(0x%x)\n", e.code());
        } catch (const hid_error &e) {
            debugf("WndProc: hid_error(0x%x)\n", e.code());
        } catch (const std::runtime_error &e) {
            debugf("WndProc: runtime_error(%s)\n", e.what());
        }
        if (handled) {
            debugf("AT_WndProcHook: handled WM_INPUT\n");
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
    }

    return CallWindowProcW(g_originalWndProcs[hWnd], hWnd, message, wParam, lParam);
}

// Hook for RegisterRawInputDevices(). Any windows that register for
// WM_INPUT mouse events will also be registered for touchpad events.
// This is necessary since an application can create many windows,
// but not all of them receive raw input.
static BOOL WINAPI
AT_RegisterRawInputDevicesHook(
    PCRAWINPUTDEVICE pRawInputDevices,
    UINT uiNumDevices,
    UINT cbSize)
{
    if (uiNumDevices > 0 &&
        pRawInputDevices[0].usUsagePage == HID_USAGE_PAGE_GENERIC &&
        pRawInputDevices[0].usUsage == HID_USAGE_GENERIC_MOUSE) {
        debugf("RegisterRawInputDevices(mouse)\n");
        try {
            AT_RegisterTouchpadInput(pRawInputDevices[0].hwndTarget);
        } catch (const win32_error &e) {
            debugf("RegisterRawInputDevices: win32_error(0x%x)\n", e.code());
        }
    } else {
        debugf("RegisterRawInputDevices(other)\n");
    }
    return g_originalRegisterRawInputDevices(pRawInputDevices, uiNumDevices, cbSize);
}

#if _WIN64

// Hook for GetWindowLongPtrW(). Ensures that applications do not see
// our fake WndProc in the stack. Only used in 64-bit builds.
static LONG_PTR WINAPI
AT_GetWindowLongPtrWHook(
    HWND hWnd,
    int nIndex)
{
    if (nIndex == GWLP_WNDPROC && g_originalWndProcs.count(hWnd)) {
        return (LONG_PTR)g_originalWndProcs[hWnd];
    }
    return g_originalGetWindowLongPtrW(hWnd, nIndex);
}

// Hook for SetWindowLongPtrW(). Ensures that applications do not overwrite
// our fake WndProc in the stack. Only used in 64-bit builds.
static LONG_PTR WINAPI
AT_SetWindowLongPtrWHook(
    HWND hWnd,
    int nIndex,
    LONG_PTR dwNewLong)
{
    if (nIndex == GWLP_WNDPROC && g_originalWndProcs.count(hWnd)) {
        WNDPROC origWndProc = g_originalWndProcs[hWnd];
        g_originalWndProcs[hWnd] = (WNDPROC)dwNewLong;
        return (LONG_PTR)origWndProc;
    }
    return g_originalSetWindowLongPtrW(hWnd, nIndex, dwNewLong);
}

#else

// Hook for GetWindowLongW(). Ensures that applications do not see
// our fake WndProc in the stack.
static LONG WINAPI
AT_GetWindowLongWHook(
    HWND hWnd,
    int nIndex)
{
    if (nIndex == GWL_WNDPROC && g_originalWndProcs.count(hWnd)) {
        return (LONG)g_originalWndProcs[hWnd];
    }
    return g_originalGetWindowLongW(hWnd, nIndex);
}

// Hook for SetWindowLongW(). Ensures that applications do not overwrite
// our fake WndProc in the stack.
static LONG WINAPI
AT_SetWindowLongWHook(
    HWND hWnd,
    int nIndex,
    LONG dwNewLong)
{
    if (nIndex == GWL_WNDPROC && g_originalWndProcs.count(hWnd)) {
        WNDPROC origWndProc = g_originalWndProcs[hWnd];
        g_originalWndProcs[hWnd] = (WNDPROC)dwNewLong;
        return (LONG)origWndProc;
    }
    return g_originalSetWindowLongW(hWnd, nIndex, dwNewLong);
}

#endif

// Hook for CreateWindowExW(). Swizzles the WndProc for the window with
// our own WndProc that will intercept WM_INPUT messages.
static HWND WINAPI
AT_CreateWindowExWHook(
    DWORD dwExStyle,
    LPCWSTR lpClassName,
    LPCWSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam)
{
    HWND hWnd = g_originalCreateWindowExW(
        dwExStyle, lpClassName, lpWindowName, dwStyle,
        X, Y, nWidth, nHeight,
        hWndParent, hMenu, hInstance, lpParam);
    debugf("CreateWindowExW() -> hwnd=%p\n", hWnd);
    WNDPROC origWndProc;
#if _WIN64
    origWndProc = (WNDPROC)g_originalSetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)AT_WndProcHook);
#else
    origWndProc = (WNDPROC)g_originalSetWindowLongW(hWnd, GWL_WNDPROC, (LONG)AT_WndProcHook);
#endif
    g_originalWndProcs[hWnd] = origWndProc;
    return hWnd;
}

// Creates a console and redirects input/output streams to it.
// Used to display debug output in non-console applications.
static void
AT_CreateConsole()
{
#if DEBUG_MODE
    FreeConsole();
    AllocConsole();
#pragma warning(push)
#pragma warning(disable:4996)
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#pragma warning(pop)
#endif
}

// Called at startup to patch all the WinAPI functions.
static void
AT_Initialize()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach((void **)&g_originalRegisterRawInputDevices, AT_RegisterRawInputDevicesHook);
    DetourAttach((void **)&g_originalGetRawInputData, AT_GetRawInputDataHook);
    DetourAttach((void **)&g_originalCreateWindowExW, AT_CreateWindowExWHook);
#if _WIN64
    DetourAttach((void **)&g_originalGetWindowLongPtrW, AT_GetWindowLongPtrWHook);
    DetourAttach((void **)&g_originalSetWindowLongPtrW, AT_SetWindowLongPtrWHook);
#else
    DetourAttach((void **)&g_originalGetWindowLongW, AT_GetWindowLongWHook);
    DetourAttach((void **)&g_originalSetWindowLongW, AT_SetWindowLongWHook);
#endif
    if (DetourTransactionCommit() != NO_ERROR) {
        throw std::runtime_error("Failed to commit Detours transaction");
    }
}

// Called at shutdown to unpatch all the WinAPI functions
// and restore any swizzled WndProcs.
static void
AT_Uninitialize()
{
    // Restore original WndProc functions
    for (auto &wndproc : g_originalWndProcs) {
#if _WIN64
        g_originalSetWindowLongPtrW(wndproc.first, GWLP_WNDPROC, (LONG_PTR)wndproc.second);
#else
        g_originalSetWindowLongW(wndproc.first, GWL_WNDPROC, (LONG)wndproc.second);
#endif
    }
    g_originalWndProcs.clear();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach((void **)&g_originalRegisterRawInputDevices, AT_RegisterRawInputDevicesHook);
    DetourDetach((void **)&g_originalGetRawInputData, AT_GetRawInputDataHook);
    DetourDetach((void **)&g_originalCreateWindowExW, AT_CreateWindowExWHook);
#if _WIN64
    DetourDetach((void **)&g_originalGetWindowLongPtrW, AT_GetWindowLongPtrWHook);
    DetourDetach((void **)&g_originalSetWindowLongPtrW, AT_SetWindowLongPtrWHook);
#else
    DetourDetach((void **)&g_originalGetWindowLongW, AT_GetWindowLongWHook);
    DetourDetach((void **)&g_originalSetWindowLongW, AT_SetWindowLongWHook);
#endif
    if (DetourTransactionCommit() != NO_ERROR) {
        throw std::runtime_error("Failed to commit Detours transaction");
    }
}

BOOL APIENTRY
DllMain(HINSTANCE hModule, DWORD dwReason, PVOID lpReserved)
{
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        AT_CreateConsole();
        AT_Initialize();
        return TRUE;
    case DLL_PROCESS_DETACH:
        AT_Uninitialize();
        return TRUE;
    default:
        return TRUE;
    }
}
