// Win32 resource ids for yip-app.
//
// Included by both yip-app.rc and C++, so it stays plain #define + include
// guards: rc.exe's preprocessor is not a C++ one.
//
// The shell picks the numerically lowest icon resource as the file's icon, so
// IDI_YIP_APP stays the lowest id here.

#ifndef YIP_APP_RESOURCE_H
#define YIP_APP_RESOURCE_H

#define IDI_YIP_APP 101

// Notification-area glyphs. Drawn for 16px and up only. The tile reads as a muddy
// square at tray sizes, which is why these exist as separate cuts.
#define IDI_YIP_TRAY_IDLE 102
#define IDI_YIP_TRAY_RECORDING 103

#endif // YIP_APP_RESOURCE_H
