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

    DWORD code()
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

    NTSTATUS code()
    {
        return m_errorCode;
    }

private:
    NTSTATUS m_errorCode;
};

// Wrapper for malloc with unique_ptr semantics, to allow
// for variable-sized structures.
struct free_deleter { void operator()(void *ptr) { free(ptr); } };
template<class T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

// Device information, such as touch area bounds and HID offsets.
// This can be reused across HID events, so we only have to parse
// this info once.
struct at_device_info
{
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData;
    RECT touchArea;
    int linkX;
    int linkY;
    int linkTouches;
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
    std::vector<HIDP_VALUE_CAPS> valueCaps(caps.NumberInputValueCaps);
    status = HidP_GetValueCaps(HidP_Input, &valueCaps[0], &caps.NumberInputValueCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw hid_error(status);
    }
    return valueCaps;
}

// Reads a single HID report value.
static ULONG
AT_GetHidUsageValue(
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
        HidP_Input,
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
    
    at_device_info &dev = g_devices[hdr.hDevice];
    dev.linkX = -1;
    dev.linkY = -1;
    dev.linkTouches = -1;
    dev.preparsedData = AT_GetHidPreparsedData(hdr);

    std::vector<HIDP_VALUE_CAPS> valueCaps = AT_GetHidInputValueCaps(dev.preparsedData.get());
    for (const HIDP_VALUE_CAPS &cap : valueCaps) {
        if (dev.linkTouches < 0 &&
            !cap.IsRange &&
            cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
            cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
            dev.linkTouches = cap.LinkCollection;
        } else if (!cap.IsRange && cap.IsAbsolute && cap.UsagePage == HID_USAGE_PAGE_GENERIC) {
            if (dev.linkX < 0 && cap.NotRange.Usage == HID_USAGE_GENERIC_X) {
                dev.touchArea.left = cap.LogicalMin;
                dev.touchArea.right = cap.LogicalMax;
                dev.linkX = cap.LinkCollection;
            } else if (dev.linkY < 0 && cap.NotRange.Usage == HID_USAGE_GENERIC_Y) {
                dev.touchArea.top = cap.LogicalMin;
                dev.touchArea.bottom = cap.LogicalMax;
                dev.linkY = cap.LinkCollection;
            }
        }

        if (dev.linkX >= 0 && dev.linkY >= 0 && dev.linkTouches >= 0) {
            break;
        }
    }

    if (dev.linkX < 0 || dev.linkY < 0 || dev.linkTouches < 0) {
        throw std::runtime_error("Usage for X/Y/contact count not found");
    }

    return dev;
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
        return false;
    }
    
    at_device_info &dev = AT_GetDeviceInfo(hdr);
    malloc_ptr<RAWINPUT> input = AT_GetRawInput(hInput, hdr);
    DWORD sizeHid = input->data.hid.dwSizeHid;
    DWORD count = input->data.hid.dwCount;
    BYTE *rawData = input->data.hid.bRawData;
    if (count == 0) {
        return true;
    }
    
    ULONG numTouches = AT_GetHidUsageValue(
        HidP_Input,
        HID_USAGE_PAGE_DIGITIZER,
        dev.linkTouches,
        HID_USAGE_DIGITIZER_CONTACT_COUNT,
        dev.preparsedData.get(),
        rawData,
        sizeHid);
    
    if (numTouches == 0) {
        return true;
    }

    ULONG x = AT_GetHidUsageValue(
        HidP_Input,
        HID_USAGE_PAGE_GENERIC,
        dev.linkX,
        HID_USAGE_GENERIC_X,
        dev.preparsedData.get(),
        rawData,
        sizeHid);

    ULONG y = AT_GetHidUsageValue(
        HidP_Input,
        HID_USAGE_PAGE_GENERIC,
        dev.linkY,
        HID_USAGE_GENERIC_Y,
        dev.preparsedData.get(),
        rawData,
        sizeHid);

    POINT touchpadPoint;
    touchpadPoint.x = x;
    touchpadPoint.y = y;
    POINT screenPoint = AT_TouchpadToScreen(dev.touchArea, touchpadPoint);

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
    debugf("GetRawInputData(handle=%x, command=%d)\n", hRawInput, uiCommand);

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
    if (message == WM_INPUT) {
        debugf("WndProc(message=WM_INPUT, hwnd=%x)\n", hWnd);
        if (AT_HandleRawInput(&wParam, &lParam)) {
            debugf("AT_WndProcHook: handled WM_INPUT\n");
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
    } else {
        debugf("WndProc(message=%d)\n", message);
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
        AT_RegisterTouchpadInput(pRawInputDevices[0].hwndTarget);
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
    debugf("GetWindowLongPtrW(hwnd=%x, index=%d)\n", hWnd, nIndex);
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
    debugf("SetWindowLongPtrW(hwnd=%x, index=%d, new=%x)\n", hWnd, nIndex, dwNewLong);
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
    debugf("GetWindowLongW(hwnd=%x, index=%d)\n", hWnd, nIndex);
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
    debugf("SetWindowLongW(hwnd=%x, index=%d, new=%x)\n", hWnd, nIndex, dwNewLong);
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
    debugf("CreateWindowExW() -> hwnd=%x\n", hWnd);
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
