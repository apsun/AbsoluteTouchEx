# AbsoluteTouchEx

AbsoluteTouchEx lets you use your touchpad like a touchscreen, giving you
absolute cursor movement just like you would get on a tablet. It is the
next generation of [AbsoluteTouch](https://github.com/apsun/AbsoluteTouch).

This project solves two major shortcomings of AbsoluteTouch:

- It only worked with Synaptics touchpads
- It was incredibly laggy on slower computers

AbsoluteTouchEx is compatible with almost any Windows precision touchpad,
and is orders of magnitude faster than AbsoluteTouch.

## WARNING

AbsoluteTouchEx, unlike AbsoluteTouch, behaves very much like a hack.
It's able to achieve its blazing fast performance by injecting itself
into the target process and hooking some Windows API calls to translate
HID events into mouse events. In contrast, AbsoluteTouch ran in its own
process and set the cursor position globally (that's why you had to
disable raw input when using AbsoluteTouch). As a result, AbsoluteTouchEx
may trigger some anti-cheat protection systems. I am not responsible if
you are banned for using AbsoluteTouchEx.

## Running the project

Make sure you have the Visual C++ 2017 Redistributable installed. You can
download the 32-bit version [here](https://aka.ms/vs/15/release/vc_redist.x86.exe)
and the 64-bit version [here](https://aka.ms/vs/15/release/vc_redist.x64.exe).

Choose the correct bitness of AbsoluteTouchEx to run. You must run the
version with the same bitness as the program that you are injecting it
into, NOT the bitness of your operating system! x86 is for 32-bit programs
and x64 is for 64-bit programs.

Ensure `atloader.exe` and `atdll.dll` are in the same directory, then
run the following command:
```
atloader.exe <path to exe to load>
```

For example, to run osu! (which, for the record, is a 32-bit program):
```
atloader.exe %LocalAppData%\osu!\osu!.exe
```

Initially at program startup, absolute touch mode will be disabled.
You can toggle it on and off by pressing `SHIFT + F6`. Make sure to enable
raw input mode; AbsoluteTouchEx will not work without it.

## Building the project

Requirements:

- Visual Studio 2017
- Windows SDK and WDK (for HID libraries)
- [Detours](https://github.com/Microsoft/Detours)

The project should open and build with no configuration necessary, assuming
you correctly installed the dependencies above. A prebuilt version of Detours
is included in the source directory; if you wish to update it you are
responsible for building it yourself.
