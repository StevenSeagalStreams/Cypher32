#pragma once
#include <RadioLib.h>
#include "cypher32_packets.h"

// ─────────────────────────────────────────────
//  CYPHER32 LORA — v29
//  Correct RadioLib pattern for SX1262/Heltec Wireless Paper
//
//  KEY FIX: use setPacketReceivedAction() not setDio1Action()
//  Per RadioLib docs: getPacketLength() MUST come before readData()
//  Per Heltec community: clearPacketReceivedAction() before TX
// ─────────────────────────────────────────────

#define LORA_MOSI  10
#define LORA_MISO  11
#define LORA_SCK    9

SPIClass loraSPI(FSPI);
// Standard RadioLib syntax — "= new Module(...)" is correct per RadioLib docs
// The constructor does NOT access SPI — it only stores pin numbers
// SPI access happens in radio.begin() which is called in loraSetup()
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSPI);

volatile bool loraRxFlag = false;
void IRAM_ATTR loraRxISR() { loraRxFlag = true; }

KnownNode knownNodes[MAX_KNOWN_NODES];
int       knownCount      = 0;
bool      loraReady       = false;
int       loraLastRSSI    = 0;
String    loraStatus      = "Offline";
int       loraInitError   = 0;
int       loraPktSent     = 0;
int       loraPktRecv     = 0;
int       loraBeaconsSent = 0;
int       loraLastPktLen  = 0;
uint8_t   loraLastPktType = 0;
String    pendingMsg      = "";
String    pendingMsgFrom  = "";

extern uint32_t myChipID32;
extern String   myFaction;
extern int      myLevel;
extern int      skillBrute, skillStealth, skillFirewall;

#define LORA_ENCRYPT 0
void loraEncrypt(uint8_t* buf, int len) { (void)buf; (void)len; }
void loraDecrypt(uint8_t* buf, int len) { (void)buf; (void)len; }

KnownNode* findOrAddNode(uint32_t chip_id) {
  for (int i = 0; i < knownCount; i++)
    if (knownNodes[i].chip_id == chip_id) return &knownNodes[i];
  if (knownCount >= MAX_KNOWN_NODES) {
    int oldest = 0;
    for (int i = 1; i < knownCount; i++)
      if (knownNodes[i].last_seen_ms < knownNodes[oldest].last_seen_ms) oldest = i;
    memset(&knownNodes[oldest], 0, sizeof(KnownNode));
    knownNodes[oldest].chip_id = chip_id;
    knownNodes[oldest].faction = '?';
    return &knownNodes[oldest];
  }
  KnownNode* n = &knownNodes[knownCount++];
  memset(n, 0, sizeof(KnownNode));
  n->chip_id = chip_id; n->faction = '?';
  return n;
}

KnownNode* findNode(uint32_t chip_id) {
  for (int i = 0; i < knownCount; i++)
    if (knownNodes[i].chip_id == chip_id) return &knownNodes[i];
  return nullptr;
}

String chipIdStr(uint32_t id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)id);
  return String(buf);
}

bool loraSend(void* pkt, int len) {
  if (!loraReady || len > 64) return false;
  uint8_t buf[64];
  memcpy(buf, pkt, len);
  // Stop RX cleanly before TX
  radio.clearPacketReceivedAction();
  radio.standby();
  int state = radio.transmit(buf, len);
  radio.setPacketReceivedAction(loraRxISR);
  radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) { loraPktSent++; return true; }
  return false;
}

void loraSendBeacon() {
  if (!loraReady) return;
  PktBeacon pkt;
  pkt.hdr.type = PKT_BEACON; pkt.hdr.from_id = myChipID32; pkt.hdr.to_id = 0;
  pkt.level    = (uint8_t)myLevel;
  pkt.faction  = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
  if (loraSend(&pkt, sizeof(pkt))) loraBeaconsSent++;
}

void loraSendRecon(uint32_t target_id) {
  PktReconReq pkt;
  pkt.hdr.type = PKT_RECON_REQ; pkt.hdr.from_id = myChipID32; pkt.hdr.to_id = target_id;
  loraSend(&pkt, sizeof(pkt));
}

void loraSendReconStat(uint32_t target_id, uint8_t wantedStat) {
  // wantedStat: 1=Brute, 2=Stealth, 3=Firewall, 0=random
  // Sends standard recon request — UI prevents duplicate stat requests
  (void)wantedStat;
  loraSendRecon(target_id);
}

void loraSendMsg(uint32_t target_id, const char* text) {
  PktMsg pkt;
  pkt.hdr.type = PKT_MSG; pkt.hdr.from_id = myChipID32; pkt.hdr.to_id = target_id;
  strncpy(pkt.text, text, 32); pkt.text[32] = '\0';
  loraSend(&pkt, sizeof(pkt));
}

void loraSendHackReq(uint32_t target_id) {
  PktHackReq pkt;
  pkt.hdr.type    = PKT_HACK_REQ;
  pkt.hdr.from_id = myChipID32;
  pkt.hdr.to_id   = target_id;
  pkt.brute       = (uint8_t)skillBrute;  // send our brute stat
  loraSend(&pkt, sizeof(pkt));
}

void loraSendHackReply(uint32_t attacker_id) {
  PktHackReply pkt;
  pkt.hdr.type = PKT_HACK_REPLY; pkt.hdr.from_id = myChipID32; pkt.hdr.to_id = attacker_id;
  pkt.firewall = (uint8_t)skillFirewall;
  pkt.faction  = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
  loraSend(&pkt, sizeof(pkt));
}

void loraSendHackResult(uint32_t defender_id, bool won, int8_t xp) {
  PktHackResult pkt;
  pkt.hdr.type = PKT_HACK_RESULT; pkt.hdr.from_id = myChipID32; pkt.hdr.to_id = defender_id;
  pkt.outcome = won ? HACK_WIN : HACK_LOSE; pkt.xp_delta = xp;
  loraSend(&pkt, sizeof(pkt));
}

void loraHandlePacket(uint8_t* buf, int len) {
  loraLastPktLen = len;
  if (len < (int)sizeof(PktHeader)) return;
  PktHeader* hdr = (PktHeader*)buf;
  loraLastPktType = hdr->type;
  if (hdr->from_id == myChipID32) return;
  loraPktRecv++;
  if (hdr->to_id != 0 && hdr->to_id != myChipID32) return;

  switch (hdr->type) {
    case PKT_BEACON: {
      if (len < (int)sizeof(PktBeacon)) return;
      PktBeacon* p = (PktBeacon*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      n->level = p->level; n->faction = (char)p->faction; n->last_seen_ms = millis();
      break;
    }
    case PKT_RECON_REQ: {
      if (len < (int)sizeof(PktReconReq)) return;
      uint8_t stats[3] = {(uint8_t)skillBrute,(uint8_t)skillStealth,(uint8_t)skillFirewall};
      uint8_t types[3] = {STAT_BRUTE,STAT_STEALTH,STAT_FIREWALL};
      int pick = random(0,3);
      PktReconReply reply;
      reply.hdr.type = PKT_RECON_REPLY; reply.hdr.from_id = myChipID32; reply.hdr.to_id = hdr->from_id;
      reply.stat_type = types[pick]; reply.stat_value = stats[pick];
      loraSend(&reply, sizeof(reply));
      break;
    }
    case PKT_RECON_REPLY: {
      if (len < (int)sizeof(PktReconReply)) return;
      PktReconReply* p = (PktReconReply*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      for (int i = 0; i < n->recon_count; i++)
        if (n->recon_types[i] == p->stat_type) return;
      if (n->recon_count < 3) {
        n->recon_types[n->recon_count]  = p->stat_type;
        n->recon_values[n->recon_count] = p->stat_value;
        n->recon_count++;
      }
      n->last_seen_ms = millis();
      break;
    }
    case PKT_HACK_REQ: {
      if (len < (int)sizeof(PktHackReq)) return;
      loraSendHackReply(hdr->from_id);
      KnownNode* n = findOrAddNode(hdr->from_id);
      if (n) n->last_seen_ms = millis();
      break;
    }
    case PKT_HACK_REPLY: {
      if (len < (int)sizeof(PktHackReply)) return;
      PktHackReply* p = (PktHackReply*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      n->faction = (char)p->faction; n->last_seen_ms = millis();
      if (n->recon_count < 3) {
        n->recon_types[n->recon_count]  = STAT_FIREWALL;
        n->recon_values[n->recon_count] = p->firewall;
        n->recon_count++;
      }
      break;
    }
    case PKT_HACK_RESULT: {
      if (len < (int)sizeof(PktHackResult)) return;
      KnownNode* n = findOrAddNode(hdr->from_id);
      if (n) n->last_seen_ms = millis();
      break;
    }
    case PKT_MSG: {
      if (len < (int)sizeof(PktMsg)) return;
      PktMsg* p = (PktMsg*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (n) {
        strncpy(n->msg_inbox, p->text, 32); n->msg_inbox[32] = '\0';
        n->msg_unread = true; n->last_seen_ms = millis();
        pendingMsg = String(p->text); pendingMsgFrom = chipIdStr(p->hdr.from_id);
      }
      break;
    }
    default: break;
  }
}

void loraTick() {
  if (!loraReady || !loraRxFlag) return;
  loraRxFlag = false;

  int pktLen = radio.getPacketLength();
  if (pktLen > 0 && pktLen <= 64) {
    uint8_t buf[64];
    int state = radio.readData(buf, (size_t)pktLen);
    if (state == RADIOLIB_ERR_NONE) {
      loraLastRSSI = (int)radio.getRSSI();
      loraHandlePacket(buf, pktLen);
    }
  }
  radio.setPacketReceivedAction(loraRxISR);
  radio.startReceive();
}

bool loraSetup() {
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  pinMode(LORA_DIO1, INPUT);
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR, LORA_PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    loraInitError = state; loraStatus = "Err:" + String(state); loraReady = false; return false;
  }
  radio.setPacketReceivedAction(loraRxISR);
  int rxState = radio.startReceive();
  if (rxState != RADIOLIB_ERR_NONE) {
    loraInitError = rxState; loraStatus = "RX err:" + String(rxState); loraReady = false; return false;
  }
  loraReady = true; loraStatus = "Online";
  return true;
}

String loraGetSignal() { return loraReady ? String(loraLastRSSI) + " dBm" : "offline"; }
String statTypeName(uint8_t t) {
  if (t == STAT_BRUTE)    return "Brute Force";
  if (t == STAT_STEALTH)  return "Stealth";
  if (t == STAT_FIREWALL) return "Firewall";
  return "Unknown";
}
char factionChar() {
  if (myFaction=="BLACK") return 'B'; if (myFaction=="WHITE") return 'W';
  if (myFaction=="RED")   return 'R'; if (myFaction=="GREEN") return 'G';
  return '?';
}
