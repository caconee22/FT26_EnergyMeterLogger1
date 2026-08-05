#include "storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"

namespace ft26::storage {
namespace {

bool mounted = false;

}  // namespace

bool beginCard() {
  SPI.begin(ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
            ft26::PIN_SD_CS);
  mounted = SD.begin(ft26::PIN_SD_CS, SPI);
  return mounted;
}

bool cardMounted() {
  return mounted;
}

uint64_t cardSizeBytes() {
  if (!mounted) {
    return 0;
  }
  return SD.cardSize();
}

}  // namespace ft26::storage
