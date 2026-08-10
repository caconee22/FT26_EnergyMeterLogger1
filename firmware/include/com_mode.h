#pragma once

namespace ft26::com_mode {

// COM 모드에서 SD 파일 목록을 준비하고 UART 명령 대기를 시작합니다.
void begin();

// COM 모드 UART ASCII 명령을 처리합니다.
void tick();

}  // namespace ft26::com_mode
