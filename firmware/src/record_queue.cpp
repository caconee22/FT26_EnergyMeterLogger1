#include "record_queue.h"

#include <Arduino.h>

#include "config.h"

namespace ft26::record_queue {
namespace {

log_format::Log records[ft26::STORAGE_QUEUE_RECORD_CAPACITY] = {};
uint16_t head = 0;
uint16_t tail = 0;
uint16_t used = 0;
uint32_t dropped = 0;
portMUX_TYPE queue_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void reset() {
  portENTER_CRITICAL(&queue_mux);
  head = 0;
  tail = 0;
  used = 0;
  dropped = 0;
  portEXIT_CRITICAL(&queue_mux);
}

bool push(const log_format::Log& record) {
  bool kept_all_records = true;
  portENTER_CRITICAL(&queue_mux);
  if (used >= ft26::STORAGE_QUEUE_RECORD_CAPACITY) {
    tail = (tail + 1) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
    --used;
    ++dropped;
    kept_all_records = false;
  }

  records[head] = record;
  head = (head + 1) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
  ++used;
  portEXIT_CRITICAL(&queue_mux);
  return kept_all_records;
}

bool empty() {
  portENTER_CRITICAL(&queue_mux);
  const bool is_empty = used == 0;
  portEXIT_CRITICAL(&queue_mux);
  return is_empty;
}

uint16_t count() {
  portENTER_CRITICAL(&queue_mux);
  const uint16_t current_count = used;
  portEXIT_CRITICAL(&queue_mux);
  return current_count;
}

size_t popBlock(log_format::Log* destination, size_t max_count) {
  if (destination == nullptr || max_count == 0) {
    return 0;
  }

  portENTER_CRITICAL(&queue_mux);
  if (used == 0) {
    portEXIT_CRITICAL(&queue_mux);
    return 0;
  }

  size_t count_to_copy = used;
  if (count_to_copy > max_count) {
    count_to_copy = max_count;
  }

  for (size_t i = 0; i < count_to_copy; ++i) {
    destination[i] = records[tail];
    tail = (tail + 1) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
  }
  used -= static_cast<uint16_t>(count_to_copy);
  portEXIT_CRITICAL(&queue_mux);
  return count_to_copy;
}

uint32_t droppedCount() {
  portENTER_CRITICAL(&queue_mux);
  const uint32_t current_dropped = dropped;
  portEXIT_CRITICAL(&queue_mux);
  return current_dropped;
}

}  // namespace ft26::record_queue
