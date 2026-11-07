#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ddp {
constexpr uint16_t PORT = 4048;
constexpr size_t PIXELS_PER_PACKET = 480;
constexpr size_t HEADER_LEN = 10;
inline void header(uint8_t *out, uint8_t sequence, uint32_t offsetBytes, uint16_t payloadBytes, bool push) {
  out[0] = uint8_t(0x40 | (push ? 0x01 : 0x00));
  out[1] = uint8_t(sequence & 0x0F);
  out[2] = 0x0B; // RGB24
  out[3] = 0x01; // display
  out[4] = uint8_t(offsetBytes >> 24);
  out[5] = uint8_t(offsetBytes >> 16);
  out[6] = uint8_t(offsetBytes >> 8);
  out[7] = uint8_t(offsetBytes);
  out[8] = uint8_t(payloadBytes >> 8);
  out[9] = uint8_t(payloadBytes);
}
}
