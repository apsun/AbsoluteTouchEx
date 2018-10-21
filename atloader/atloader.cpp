#include <exception>
#include <iostream>
#include <string>
#include <Windows.h>
#include "Detours.h"

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

int
main(int argc, char *argv[])
{
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
        throw win32_error();
    }
    
    std::string exePath(path);
    std::string exeDir = exePath.substr(0, exePath.find_last_of('\\'));
    std::string atDllPath = exeDir + "\\atdll.dll";
    std::string targetPath = exeDir + "\\attest.exe";
    if (argc >= 2) {
        targetPath = argv[1];
    }

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    if (!DetourCreateProcessWithDllExA(
        targetPath.c_str(),
        nullptr,
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &startupInfo,
        nullptr,
        atDllPath.c_str(),
        nullptr))
    {
        std::cerr << "Failed to create process: 0x" << std::hex << GetLastError() << std::endl;
    }
}
