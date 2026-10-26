#pragma once
// Windows notices for when nobody is looking at the window (the bot runs minimized): a notification (the tray
// icon's balloon, which Windows 10 shows as a toast and keeps in the Action Center) and the taskbar button flashing
// until the window comes to the front. Clicking the notification or the tray icon brings the window back
// (main.cpp wnd_proc: kTrayMsg).
#include <string>

namespace nj::notify {

constexpr unsigned kTrayMsg = 0x8000 + 77;          // WM_APP + 77: the tray icon's callback message

void init(void* hwnd);                              // the main window (its icon; the callback goes to it)
// A notification: UTF-8 title (64 chars max) and text (256 max); warning = the warning icon, else the app's.
void show(const std::string& title, const std::string& text, bool warning);
void shutdown();                                    // the tray icon off (at exit)
void bring_back();                                  // restore + bring the window to the front

} // namespace nj::notify
