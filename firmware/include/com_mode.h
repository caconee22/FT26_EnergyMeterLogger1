#pragma once

namespace ft26::com_mode {

// Start USB/UART command processing. Recording mode keeps destructive and
// long-running commands disabled so logging cannot be interrupted.
void begin(bool recording_mode = false);

// COM 모드 UART ASCII 명령을 처리합니다.
void tick();

}  // namespace ft26::com_mode
