#pragma once

#include <stddef.h>
#include <stdint.h>

#include "log_format.h"

namespace ft26::record_queue {

// Reset the RAM queue used by the SD writer.
void reset();

// Push one record. Returns false when the oldest queued record was dropped.
bool push(const log_format::Log& record);

// queue가 비어 있는지 반환합니다.
bool empty();

// queue에 쌓인 record 수를 반환합니다.
uint16_t count();

// Atomically copy and remove up to max_count records from the queue.
size_t popBlock(log_format::Log* destination, size_t max_count);

// Return the total number of records dropped by queue overflow.
uint32_t droppedCount();

}  // namespace ft26::record_queue
