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

Requirements:

- Windows 10
- Windows precision touchpad drivers

Ensure that your computer is using a Windows precision touchpad by going
to Settings -> Devices -> Touchpad. If your touchpad has precision
drivers installed, you should see "Your PC has a precision touchpad." at
the top. **If you do not see this message, AbsoluteTouchEx will not work.**

Download the AbsoluteTouchEx executable from the
[releases page](https://github.com/apsun/AbsoluteTouchEx/releases).

Choose the correct version of AbsoluteTouchEx to run. You must run the
version with the same bitness as the program that you are injecting it
into, NOT the bitness of your operating system! x86 is for 32-bit programs
and x64 is for 64-bit programs.

Make sure you have the Visual C++ 2019 Redistributable installed (again,
for the bitness version that you intend to run, not for the bitness of
your operating system). You can download the 32-bit version
[here](https://aka.ms/vs/16/release/vc_redist.x86.exe) and the 64-bit
version [here](https://aka.ms/vs/16/release/vc_redist.x64.exe).

Extract `atloader.exe` and `atdll.dll` to the same directory, then
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

To adjust the area of your touchpad that gets mapped to the screen, press
`SHIFT + F7` to enter calibration mode. Draw a rectangle on your touchpad
around the area that you wish to use (simply touching the top-left and
bottom-right corners is also sufficient), then press `SHIFT + F7` again to
save. Note that this must be done every time AbsoluteTouchEx is run; your
settings are not saved to disk. While in calibration mode, your cursor
will not move; that is normal.

## Building the project

Requirements:

- Visual Studio 2019
- Windows 10 SDK and WDK (for HID libraries)

The project should open and build with no configuration necessary, assuming
you correctly installed the dependencies above. A prebuilt version of
[Detours](https://github.com/Microsoft/Detours) is included in the source
directory; if you wish to update it you are responsible for building it
yourself.
