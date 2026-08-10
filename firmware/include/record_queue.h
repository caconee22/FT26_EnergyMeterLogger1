#pragma once

#include <stddef.h>
#include <stdint.h>

#include "log_format.h"

namespace ft26::record_queue {

// SD writer가 사용할 RAM record queue를 초기화합니다.
void reset();

// queue에 record 1개를 넣습니다. 가득 차 있으면 가장 오래된 record를 버립니다.
bool push(const log_format::Log& record);

// queue가 비어 있는지 반환합니다.
bool empty();

// queue에 쌓인 record 수를 반환합니다.
uint16_t count();

// SD에 바로 쓸 수 있는 연속 record 영역을 반환합니다.
const log_format::Log* readBlock(size_t max_count, size_t& out_count);

// SD에 기록한 record 수만큼 queue에서 제거합니다.
void pop(size_t written_count);

}  // namespace ft26::record_queue
