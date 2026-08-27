#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────
//  CYPHER32 — minimal QR encoder
// ─────────────────────────────────────────────
//
//  Just enough QR to put a Wi-Fi join string on the e-ink:
//  byte mode, error correction level L, versions 1-4 (21x21 to 33x33).
//
//  WHY NOT A LIBRARY
//  There is no ESP32 toolchain in this repo's test environment, so an added
//  dependency could not be compiled, let alone checked for correctness. This
//  is ~250 lines with no includes beyond Arduino, and the host test compares
//  its output matrix cell-by-cell against Python's `qrcode` package for a
//  spread of inputs. A wrong QR that still renders is worse than none.
//
//  Levels 1-4 at ECC L all use exactly one error-correction block, which is
//  why there is no interleaving here. That stops being true at version 5.

#define QR_MAX_VERSION 4
#define QR_MAX_SIZE    (17 + 4 * QR_MAX_VERSION)   // 33

// Per version, ECC level L: data codewords and EC codewords.
static const uint8_t QR_DATA_CW[5] = { 0, 19, 34, 55, 80 };
static const uint8_t QR_ECC_CW [5] = { 0,  7, 10, 15, 20 };
// Second alignment-pattern centre (the first is always 6). 0 = none.
static const uint8_t QR_ALIGN  [5] = { 0,  0, 18, 22, 26 };

// Test hook only: pin the mask so a reference implementation can be compared
// on data placement alone. -1 selects by penalty score, as the spec requires.
static int qrForceMask = -1;

struct QrCode {
  uint8_t size;                              // modules per side
  uint8_t mod[QR_MAX_SIZE][QR_MAX_SIZE];     // 1 = dark
};

// ── GF(256), primitive polynomial 0x11D ──
static uint8_t qrExp[512], qrLog[256];
static bool    qrTablesReady = false;

static void qrInitTables() {
  if (qrTablesReady) return;
  int x = 1;
  for (int i = 0; i < 255; i++) {
    qrExp[i] = (uint8_t)x;
    qrLog[x] = (uint8_t)i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11D;
  }
  for (int i = 255; i < 512; i++) qrExp[i] = qrExp[i - 255];
  qrTablesReady = true;
}

static uint8_t qrMul(uint8_t a, uint8_t b) {
  if (!a || !b) return 0;
  return qrExp[qrLog[a] + qrLog[b]];
}

// Reed-Solomon: append `ecLen` codewords to `data` (length `dataLen`).
static void qrReedSolomon(const uint8_t* data, int dataLen, int ecLen, uint8_t* out) {
  qrInitTables();
  uint8_t gen[31];
  memset(gen, 0, sizeof(gen));
  gen[0] = 1;
  int genLen = 1;
  for (int i = 0; i < ecLen; i++) {                 // gen *= (x - a^i)
    for (int j = genLen; j > 0; j--)
      gen[j] = (uint8_t)(gen[j - 1] ^ qrMul(gen[j], qrExp[i]));
    gen[0] = qrMul(gen[0], qrExp[i]);
    genLen++;
  }
  memset(out, 0, ecLen);
  for (int i = 0; i < dataLen; i++) {
    uint8_t factor = (uint8_t)(data[i] ^ out[0]);
    memmove(out, out + 1, ecLen - 1);
    out[ecLen - 1] = 0;
    for (int j = 0; j < ecLen; j++)
      out[j] = (uint8_t)(out[j] ^ qrMul(gen[ecLen - 1 - j], factor));
  }
}

// ── module helpers ──
static void qrSet(QrCode* q, int r, int c, uint8_t v) {
  if (r < 0 || c < 0 || r >= q->size || c >= q->size) return;
  q->mod[r][c] = v;
}

static void qrDrawFinder(QrCode* q, uint8_t* res, int r0, int c0) {
  for (int r = -1; r <= 7; r++)
    for (int c = -1; c <= 7; c++) {
      int rr = r0 + r, cc = c0 + c;
      if (rr < 0 || cc < 0 || rr >= q->size || cc >= q->size) continue;
      bool dark = (r >= 0 && r <= 6 && (c == 0 || c == 6)) ||
                  (c >= 0 && c <= 6 && (r == 0 || r == 6)) ||
                  (r >= 2 && r <= 4 && c >= 2 && c <= 4);
      q->mod[rr][cc] = dark ? 1 : 0;
      res[rr * QR_MAX_SIZE + cc] = 1;
    }
}

static void qrDrawAlign(QrCode* q, uint8_t* res, int r0, int c0) {
  for (int r = -2; r <= 2; r++)
    for (int c = -2; c <= 2; c++) {
      int rr = r0 + r, cc = c0 + c;
      bool dark = (r == -2 || r == 2 || c == -2 || c == 2 || (r == 0 && c == 0));
      q->mod[rr][cc] = dark ? 1 : 0;
      res[rr * QR_MAX_SIZE + cc] = 1;
    }
}

static bool qrMaskBit(int mask, int r, int c) {
  switch (mask) {
    case 0: return ((r + c) % 2) == 0;
    case 1: return (r % 2) == 0;
    case 2: return (c % 3) == 0;
    case 3: return ((r + c) % 3) == 0;
    case 4: return (((r / 2) + (c / 3)) % 2) == 0;
    case 5: return ((r * c) % 2) + ((r * c) % 3) == 0;
    case 6: return ((((r * c) % 2) + ((r * c) % 3)) % 2) == 0;
    default:return ((((r + c) % 2) + ((r * c) % 3)) % 2) == 0;
  }
}

// Format information: 2-bit ECC level (L = 01) and 3-bit mask, BCH(15,5),
// then XOR 0x5412 so an all-zero format is never valid.
static uint16_t qrFormatBits(int mask) {
  uint16_t data = (uint16_t)((0x01 << 3) | mask);
  uint16_t rem  = data;
  for (int i = 0; i < 10; i++) rem = (uint16_t)((rem << 1) ^ (((rem >> 9) & 1) * 0x537));
  return (uint16_t)(((data << 10) | (rem & 0x3FF)) ^ 0x5412);
}

static void qrPlaceFormat(QrCode* q, int mask) {
  uint16_t bits = qrFormatBits(mask);
  #define QR_FMT(r, c, i) qrSet(q, (r), (c), (uint8_t)((bits >> (i)) & 1))

  // Copy 1 hugs the top-left finder: bits 0-5 run DOWN column 8, then the
  // rest run LEFT along row 8. Both copies are easy to transpose by accident,
  // and transposing them corrupts nothing but the format — the data region
  // still decodes, so it looks almost right while scanning not at all.
  for (int i = 0; i <= 5; i++) QR_FMT(i, 8, i);
  QR_FMT(7, 8, 6);
  QR_FMT(8, 8, 7);
  QR_FMT(8, 7, 8);
  for (int i = 9; i < 15; i++) QR_FMT(8, 14 - i, i);

  // Copy 2: bits 0-7 run LEFT along row 8 from the right edge, bits 8-14 run
  // DOWN column 8 from the bottom edge.
  for (int i = 0; i < 8; i++)  QR_FMT(8, q->size - 1 - i, i);
  for (int i = 8; i < 15; i++) QR_FMT(q->size - 15 + i, 8, i);
  #undef QR_FMT

  qrSet(q, q->size - 8, 8, 1);          // always-dark module
}

static int qrPenalty(const QrCode* q) {
  int n = q->size, score = 0;

  for (int pass = 0; pass < 2; pass++) {          // rule 1: runs of 5+
    for (int a = 0; a < n; a++) {
      int run = 1;
      for (int b = 1; b < n; b++) {
        uint8_t cur  = pass ? q->mod[b][a]     : q->mod[a][b];
        uint8_t prev = pass ? q->mod[b - 1][a] : q->mod[a][b - 1];
        if (cur == prev) { run++; }
        else { if (run >= 5) score += 3 + (run - 5); run = 1; }
      }
      if (run >= 5) score += 3 + (run - 5);
    }
  }
  for (int r = 0; r + 1 < n; r++)                 // rule 2: 2x2 blocks
    for (int c = 0; c + 1 < n; c++) {
      uint8_t v = q->mod[r][c];
      if (v == q->mod[r][c+1] && v == q->mod[r+1][c] && v == q->mod[r+1][c+1])
        score += 3;
    }
  const uint8_t p1[11] = {1,0,1,1,1,0,1,0,0,0,0};  // rule 3: finder-like
  const uint8_t p2[11] = {0,0,0,0,1,0,1,1,1,0,1};
  for (int pass = 0; pass < 2; pass++)
    for (int a = 0; a < n; a++)
      for (int b = 0; b + 10 < n; b++) {
        bool m1 = true, m2 = true;
        for (int k = 0; k < 11; k++) {
          uint8_t v = pass ? q->mod[b + k][a] : q->mod[a][b + k];
          if (v != p1[k]) m1 = false;
          if (v != p2[k]) m2 = false;
        }
        if (m1) score += 40;
        if (m2) score += 40;
      }
  // Rule 4: departure from a 50% dark ratio, one step per 5%.
  // Done in exact integers rather than by truncating the percentage first —
  // rounding early changes the step near a boundary and picks a different
  // mask, which is how this drifted from the reference.
  int dark = 0;
  for (int r = 0; r < n; r++) for (int c = 0; c < n; c++) dark += q->mod[r][c];
  int total = n * n;
  int num   = dark * 100 - 50 * total;
  if (num < 0) num = -num;
  score += (num / (5 * total)) * 10;
  return score;
}

// Encode `text` as a QR. Returns false if it does not fit in version 4.
inline bool qrEncode(const char* text, QrCode* q) {
  int len = (int)strlen(text);

  int ver = 0;
  for (int v = 1; v <= QR_MAX_VERSION; v++) {
    int cap = ((QR_DATA_CW[v] * 8) - 12) / 8;      // minus mode + 8-bit count
    if (len <= cap) { ver = v; break; }
  }
  if (!ver) return false;

  const int dataCw = QR_DATA_CW[ver], eccCw = QR_ECC_CW[ver];
  q->size = (uint8_t)(17 + 4 * ver);

  // ── bit stream ──
  uint8_t buf[80]; memset(buf, 0, sizeof(buf));
  int bit = 0;
  #define QR_PUT(val, n) for (int _i = (n) - 1; _i >= 0; _i--) {            \
      if (((val) >> _i) & 1) buf[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));  \
      bit++;                                                                  \
    }
  QR_PUT(0x4, 4);                                  // byte mode
  QR_PUT(len, 8);                                  // count (versions 1-9)
  for (int i = 0; i < len; i++) QR_PUT((uint8_t)text[i], 8);
  int cap = dataCw * 8;
  for (int i = 0; i < 4 && bit < cap; i++) { QR_PUT(0, 1); }   // terminator
  while (bit % 8) { QR_PUT(0, 1); }
  #undef QR_PUT
  for (int i = bit / 8; i < dataCw; i++)           // pad bytes
    buf[i] = ((i - bit / 8) % 2 == 0) ? 0xEC : 0x11;

  uint8_t ecc[20];
  qrReedSolomon(buf, dataCw, eccCw, ecc);
  memcpy(buf + dataCw, ecc, eccCw);
  const int totalCw = dataCw + eccCw;

  // ── fixed patterns ──
  static uint8_t res[QR_MAX_SIZE * QR_MAX_SIZE];
  memset(res, 0, sizeof(res));
  memset(q->mod, 0, sizeof(q->mod));

  qrDrawFinder(q, res, 0, 0);
  qrDrawFinder(q, res, 0, q->size - 7);
  qrDrawFinder(q, res, q->size - 7, 0);

  for (int i = 8; i < q->size - 8; i++) {          // timing
    uint8_t v = (i % 2 == 0) ? 1 : 0;
    q->mod[6][i] = v; res[6 * QR_MAX_SIZE + i] = 1;
    q->mod[i][6] = v; res[i * QR_MAX_SIZE + 6] = 1;
  }
  if (QR_ALIGN[ver]) {                             // one alignment pattern
    int a = QR_ALIGN[ver];
    qrDrawAlign(q, res, a, a);
  }
  for (int i = 0; i < 9; i++) {                    // reserve format areas
    res[8 * QR_MAX_SIZE + i] = 1;
    res[i * QR_MAX_SIZE + 8] = 1;
  }
  for (int i = 0; i < 8; i++) {
    res[8 * QR_MAX_SIZE + (q->size - 1 - i)] = 1;
    res[(q->size - 1 - i) * QR_MAX_SIZE + 8] = 1;
  }
  res[(q->size - 8) * QR_MAX_SIZE + 8] = 1;        // dark module

  // ── data, zigzag from bottom right, skipping the vertical timing column ──
  int bitIdx = 0, dir = -1, row = q->size - 1;
  for (int col = q->size - 1; col > 0; col -= 2) {
    if (col == 6) col--;                           // column 6 is timing
    while (true) {
      for (int k = 0; k < 2; k++) {
        int c = col - k;
        if (!res[row * QR_MAX_SIZE + c]) {
          uint8_t b = 0;
          if (bitIdx < totalCw * 8)
            b = (uint8_t)((buf[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1);
          q->mod[row][c] = b;
          bitIdx++;
        }
      }
      row += dir;
      if (row < 0 || row >= q->size) { row -= dir; dir = -dir; break; }
    }
  }

  // ── mask selection ──
  //
  // Score each candidate BEFORE writing its real format bits, then write them
  // into the winner. Those 31 modules are enough to change which mask wins,
  // and scoring with them included picked mask 2 (vertical stripes) for one of
  // our own Wi-Fi strings — a code OpenCV could not read at all, while seven
  // of the eight masks decoded fine. Reference implementations score with
  // placeholder format bits and their output scans, so we do the same.
  QrCode best; int bestScore = 0x7FFFFFFF; int bestMask = 0;
  for (int m = 0; m < 8; m++) {
    if (qrForceMask >= 0 && m != qrForceMask) continue;
    QrCode t = *q;
    for (int r = 0; r < t.size; r++)
      for (int c = 0; c < t.size; c++)
        if (!res[r * QR_MAX_SIZE + c] && qrMaskBit(m, r, c))
          t.mod[r][c] ^= 1;
    int sc = qrPenalty(&t);
    if (sc < bestScore) { bestScore = sc; best = t; bestMask = m; }
  }
  qrPlaceFormat(&best, bestMask);
  *q = best;
  return true;
}

// The join string phones understand. No password: the AP is open by design.
inline String qrWifiString(const String& ssid) {
  return "WIFI:T:nopass;S:" + ssid + ";;";
}
