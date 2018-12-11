# AbsoluteTouchEx

AbsoluteTouchEx is the next generation of
[AbsoluteTouch](https://github.com/apsun/AbsoluteTouch).
Ever wanted to use your laptop touchpad to play osu!?
Well, now you can!

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

## How to run

Ensure that `atloader.exe` and `atdll.dll` are in the same directory,
then run the following command:
```
atloader.exe <path to exe to load>
```

For example, to run osu!:
```
atloader.exe %LocalAppData%\osu!\osu!.exe
```

Initially at program startup, absolute touch mode will be disabled.
You can toggle it on and off by pressing `SHIFT + F6`.

## Building the project

Requirements:

- Visual Studio 2017
- Windows SDK and WDK (for HID libraries)
- [Detours](https://github.com/Microsoft/Detours)

You must build the project in the same bitness as the program you intend
to inject the DLL into. For example, to inject into a 32-bit program, build
the project in 32-bit mode.
