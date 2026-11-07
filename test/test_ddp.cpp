#include <cassert>
#include <cstdint>
#include "ddp_packet.h"
int main() {
  uint8_t h[10]{};
  ddp::header(h, 19, 1440, 1440, false);
  assert(h[0]==0x40 && h[1]==3 && h[2]==0x0B && h[3]==1);
  assert(h[4]==0 && h[5]==0 && h[6]==5 && h[7]==160);
  assert(h[8]==5 && h[9]==160);
  ddp::header(h, 2, 2880, 30, true);
  assert(h[0]==0x41 && h[1]==2);
  assert(h[6]==11 && h[7]==64 && h[8]==0 && h[9]==30);
  return 0;
}
