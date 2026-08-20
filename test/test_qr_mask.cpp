// Same dump as test_qr, but with the mask pinned so every mask can be compared.
#include <Arduino.h>
#include <cstdio>
uint32_t g_millis=0, g_rngState=1; SerialStub Serial; ESPStub ESP;
#include "../cypher32_qr.h"
int main(int argc, char** argv) {
  qrForceMask = atoi(argv[1]);
  for (int i = 2; i < argc; i++) {
    QrCode q;
    if (!qrEncode(argv[i], &q)) { printf("FIT_FAIL %s\n", argv[i]); continue; }
    printf("QR %s %d\n", argv[i], q.size);
    for (int r = 0; r < q.size; r++) {
      for (int c = 0; c < q.size; c++) putchar(q.mod[r][c] ? '#' : '.');
      putchar('\n');
    }
  }
  return 0;
}
