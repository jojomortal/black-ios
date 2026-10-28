#pragma once
// platform_apple.cpp's and platform_apple_objc.mm's shared part

namespace platform {

// once SDL is up: logs the controllers SDL sees, frees their system gestures (Share, PS, Menu) for the game, and on iOS
// watches the audio session; BLACK_DEBUG_INPUT=1 logs the raw buttons, axes and the controller's gyro
void StartAppleObservers();

// iOS: the audio session's state, with what happened (a transition of the app), in the log; nothing on the Mac
void LogAudioSession(const char* when);

}  // namespace platform
