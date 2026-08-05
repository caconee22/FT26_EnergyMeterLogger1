#pragma once

#include <stdint.h>

namespace ft26::storage {

// microSD SPI 버스를 시작하고 카드를 마운트합니다. 파일은 만들지 않습니다.
bool beginCard();

// SD 카드가 마운트되어 있는지 반환합니다.
bool cardMounted();

// 마운트된 SD 카드의 전체 용량을 바이트로 반환합니다.
uint64_t cardSizeBytes();

}  // namespace ft26::storage
