#include "record_queue.h"

#include <Arduino.h>

#include "config.h"

namespace ft26::record_queue {
namespace {

log_format::Log records[ft26::STORAGE_QUEUE_RECORD_CAPACITY] = {};
uint16_t head = 0;
uint16_t tail = 0;
uint16_t used = 0;
portMUX_TYPE queue_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void reset() {
  portENTER_CRITICAL(&queue_mux);
  head = 0;
  tail = 0;
  used = 0;
  portEXIT_CRITICAL(&queue_mux);
}

bool push(const log_format::Log& record) {
  portENTER_CRITICAL(&queue_mux);
  if (used >= ft26::STORAGE_QUEUE_RECORD_CAPACITY) {
    tail = (tail + 1) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
    --used;
  }

  records[head] = record;
  head = (head + 1) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
  ++used;
  portEXIT_CRITICAL(&queue_mux);
  return true;
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

const log_format::Log* readBlock(size_t max_count, size_t& out_count) {
  out_count = 0;
  portENTER_CRITICAL(&queue_mux);
  if (used == 0 || max_count == 0) {
    portEXIT_CRITICAL(&queue_mux);
    return nullptr;
  }

  const size_t contiguous = ft26::STORAGE_QUEUE_RECORD_CAPACITY - tail;
  out_count = used < contiguous ? used : contiguous;
  if (out_count > max_count) {
    out_count = max_count;
  }

  const log_format::Log* block = &records[tail];
  portEXIT_CRITICAL(&queue_mux);
  return block;
}

void pop(size_t written_count) {
  portENTER_CRITICAL(&queue_mux);
  if (written_count > used) {
    written_count = used;
  }

  tail = (tail + written_count) % ft26::STORAGE_QUEUE_RECORD_CAPACITY;
  used -= static_cast<uint16_t>(written_count);
  portEXIT_CRITICAL(&queue_mux);
}

}  // namespace ft26::record_queue
