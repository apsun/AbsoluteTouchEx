#include <exception>
#include <iostream>
#include <memory>
#include <Windows.h>
#include <hidusage.h>

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

// Wrapper for malloc with unique_ptr semantics, to allow
// for variable-sized structures.
struct free_deleter { void operator()(void *ptr) { free(ptr); } };
template<typename T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

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

// Reads the raw input header for the given raw input handle.
static RAWINPUTHEADER
AT_GetRawInputHeader(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr;
    UINT size = sizeof(hdr);
    if (GetRawInputData(hInput, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
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
    if (GetRawInputData(hInput, RID_INPUT, input.get(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw win32_error();
    }
    return input;
}

// Handles raw mouse input. Prints the coordinates to stdout.
static void
AT_HandleRawInput(WPARAM wParam, LPARAM lParam)
{
    HRAWINPUT hInput = (HRAWINPUT)lParam;
    RAWINPUTHEADER hdr = AT_GetRawInputHeader(hInput);
    if (hdr.dwType != RIM_TYPEMOUSE) {
        return;
    }

    malloc_ptr<RAWINPUT> input = AT_GetRawInput(hInput, hdr);
    if (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        LONG x = input->data.mouse.lLastX;
        LONG y = input->data.mouse.lLastY;
        std::cout << x << ", " << y << std::endl;
    }
}

// The "real" WndProc.
static LRESULT CALLBACK
AT_WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_INPUT:
        AT_HandleRawInput(wParam, lParam);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Registers the main window class.
static ATOM
AT_RegisterClass(HINSTANCE hInstance, LPCWSTR className)
{
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = AT_WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = nullptr;
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = className;
    wcex.hIconSm = nullptr;
    return RegisterClassExW(&wcex);
}

// Creates an invisible window for receiving messages.
static HWND
AT_CreateWindow(HINSTANCE hInstance, LPCWSTR className, LPCWSTR title)
{
    return CreateWindowW(
        className,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        0,
        CW_USEDEFAULT,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
}

int
main()
{
    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    AT_RegisterClass(hInstance, L"ATWndCls");
    HWND hWnd = AT_CreateWindow(hInstance, L"ATWndCls", L"AbsoluteTouch Test");

    RAWINPUTDEVICE dev;
    dev.usUsagePage = HID_USAGE_PAGE_GENERIC;
    dev.usUsage = HID_USAGE_GENERIC_MOUSE;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hWnd;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE))) {
        throw win32_error();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
