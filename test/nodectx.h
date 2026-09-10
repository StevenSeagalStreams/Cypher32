// One device's entire link-layer state, lifted out of test_twonode.cpp so the
// three-node mesh test can drive the same model. The firmware's globals ARE
// the device, so "running two devices" means swapping the whole set in and out
// around each tick — which is also why anything added to the firmware has to
// be added here, or it silently bleeds between devices.
#pragma once

// ── snapshot of one device's entire link state ──
struct NodeCtx {
  TxFrame    txq_[TXQ_SIZE];
  SeenEntry  seen_[SEEN_RING_SIZE];
  int        seenIdx_;
  PendingTx  pendingU_, pendingR_;
  KnownNode  nodes_[MAX_KNOWN_NODES];
  int        knownCount_;
  uint8_t    txSeq_;
  RadioState rs_;
  uint32_t   txStart_;
  bool       dio_;
  uint32_t   chipId_;
  char       fac_[8];          // plain POD — this struct gets bulk-zeroed
  int        lvl_, brute_, stealth_, fw_;
  int        acksRecv_, acksSent_, timeouts_, retries_, dups_, pktSent_, pktRecv_, cad_;
  LoraActionState action_;
  // hack state (T4.3) — per device, must not bleed between the two instances
  bool       hackAlert_, hackAlertWon_, hackInFlight_, hackVerdictReady_;
  bool       hackVerdictWon_, hackTimedOut_, hackVerdictLate_;
  uint32_t   hackTarget_, hackGrace_;
  ReconLedgerEntry ledger_[RECON_LEDGER_SIZE];
  uint8_t    ledgerIdx_;
  uint8_t    hackVerdictFw_;
  char        hackFrom_[16];
  std::vector<uint8_t> rxbuf_;
  size_t     sentSeen_;      // how many of radio.sent we've already drained
};

void save(NodeCtx& c) {
  memcpy(c.txq_, txq, sizeof(txq));
  memcpy(c.seen_, seenRing, sizeof(seenRing)); c.seenIdx_ = seenIdx;
  c.pendingU_ = pendingUser; c.pendingR_ = pendingReply;
  memcpy(c.nodes_, knownNodes, sizeof(knownNodes)); c.knownCount_ = knownCount;
  c.txSeq_ = txSeq; c.rs_ = radioState; c.txStart_ = txStartMs; c.dio_ = loraDioFlag;
  c.chipId_ = myChipID32;
  strncpy(c.fac_, myFaction.c_str(), sizeof(c.fac_) - 1); c.fac_[sizeof(c.fac_) - 1] = '\0';
  c.lvl_ = myLevel; c.brute_ = skillBrute; c.stealth_ = skillStealth; c.fw_ = skillFirewall;
  c.acksRecv_ = loraAcksRecv; c.acksSent_ = loraAcksSent; c.timeouts_ = loraTimeouts;
  c.retries_ = loraRetries;   c.dups_ = loraDupsDropped;
  c.pktSent_ = loraPktSent;   c.pktRecv_ = loraPktRecv; c.cad_ = loraCadBusy;
  c.action_ = loraActionState;
  c.hackAlert_ = pendingHackAlert; c.hackAlertWon_ = pendingHackAttackerWon;
  c.hackInFlight_ = hackInFlight;  c.hackVerdictReady_ = hackVerdictReady;
  c.hackVerdictWon_ = hackVerdictWon; c.hackTimedOut_ = hackTimedOut;
  c.hackVerdictLate_ = hackVerdictLate; c.hackGrace_ = hackGraceUntil;
  memcpy(c.ledger_, reconLedger, sizeof(reconLedger)); c.ledgerIdx_ = reconLedgerIdx;
  c.hackTarget_ = hackTargetId;    c.hackVerdictFw_ = hackVerdictFirewall;
  strncpy(c.hackFrom_, pendingHackFrom.c_str(), sizeof(c.hackFrom_) - 1);
  c.hackFrom_[sizeof(c.hackFrom_) - 1] = '\0';
  c.rxbuf_ = radio.rxBuf;
}

void load(NodeCtx& c) {
  memcpy(txq, c.txq_, sizeof(txq));
  memcpy(seenRing, c.seen_, sizeof(seenRing)); seenIdx = c.seenIdx_;
  pendingUser = c.pendingU_; pendingReply = c.pendingR_;
  memcpy(knownNodes, c.nodes_, sizeof(knownNodes)); knownCount = c.knownCount_;
  txSeq = c.txSeq_; radioState = c.rs_; txStartMs = c.txStart_; loraDioFlag = c.dio_;
  myChipID32 = c.chipId_; myFaction = String(c.fac_);
  myLevel = c.lvl_; skillBrute = c.brute_; skillStealth = c.stealth_; skillFirewall = c.fw_;
  loraAcksRecv = c.acksRecv_; loraAcksSent = c.acksSent_; loraTimeouts = c.timeouts_;
  loraRetries = c.retries_;   loraDupsDropped = c.dups_;
  loraPktSent = c.pktSent_;   loraPktRecv = c.pktRecv_; loraCadBusy = c.cad_;
  loraActionState = c.action_;
  pendingHackAlert = c.hackAlert_; pendingHackAttackerWon = c.hackAlertWon_;
  hackInFlight = c.hackInFlight_;  hackVerdictReady = c.hackVerdictReady_;
  hackVerdictWon = c.hackVerdictWon_; hackTimedOut = c.hackTimedOut_;
  hackVerdictLate = c.hackVerdictLate_; hackGraceUntil = c.hackGrace_;
  memcpy(reconLedger, c.ledger_, sizeof(reconLedger)); reconLedgerIdx = c.ledgerIdx_;
  hackTargetId = c.hackTarget_;    hackVerdictFirewall = c.hackVerdictFw_;
  pendingHackFrom = String(c.hackFrom_);
  radio.rxBuf = c.rxbuf_;
  radio.sent.clear();
  loraReady = true;
}

void initCtx(NodeCtx& c, uint32_t id, const char* fac, int brute, int fw) {
  // Zero the POD members individually — NodeCtx holds a std::vector, so a
  // blanket memset over the whole struct would corrupt it.
  memset(c.txq_,   0, sizeof(c.txq_));
  memset(c.seen_,  0, sizeof(c.seen_));   c.seenIdx_    = 0;
  memset(&c.pendingU_, 0, sizeof(c.pendingU_));
  memset(&c.pendingR_, 0, sizeof(c.pendingR_));
  memset(c.nodes_, 0, sizeof(c.nodes_));  c.knownCount_ = 0;
  c.txSeq_ = 0; c.rs_ = RS_RX; c.txStart_ = 0; c.dio_ = false;
  c.chipId_ = id;
  strncpy(c.fac_, fac, sizeof(c.fac_) - 1); c.fac_[sizeof(c.fac_) - 1] = '\0';
  c.lvl_ = 5; c.brute_ = brute; c.stealth_ = 4; c.fw_ = fw;
  c.acksRecv_ = c.acksSent_ = c.timeouts_ = c.retries_ = 0;
  c.dups_ = c.pktSent_ = c.pktRecv_ = c.cad_ = 0;
  c.action_ = LA_IDLE;
  c.hackAlert_ = c.hackAlertWon_ = c.hackInFlight_ = false;
  c.hackVerdictReady_ = c.hackVerdictWon_ = c.hackTimedOut_ = false;
  c.hackVerdictLate_ = false; c.hackGrace_ = 0;
  memset(c.ledger_, 0, sizeof(c.ledger_)); c.ledgerIdx_ = 0;
  c.hackTarget_ = 0; c.hackVerdictFw_ = 0; c.hackFrom_[0] = '\0';
  c.rxbuf_.clear();
  c.sentSeen_ = 0;
}


// Everything reachable beyond direct range. Kept next to the rest of the state
// for the same reason: an echo table that leaked between simulated devices
// would make the mesh look like it works when it does not.
struct MeshCtx {
  EchoNode echo_[ECHO_MAX_NODES];
  int      echoCount_;
  uint32_t nextEcho_;
  MailItem mail_[MAIL_SLOTS];
  bool     msgRelayed_;
  uint32_t courierFor_;
  int      echoSent_, echoRecv_, mailDeliv_, mailHanded_, mailExp_;
  char     pendingMsg_[40], pendingFrom_[16];
  // Scheduler and airtime state. These are firmware globals that NodeCtx never
  // captured, because test_twonode always sent its beacons by hand and so never
  // exercised them. Three devices sharing one beacon clock transmit nothing at
  // all, which is a convincing way for a mesh test to report success.
  bool     beaconEnabled_;
  uint32_t nextBeacon_, fastUntil_;
  uint8_t  bootBurst_;
  uint16_t dutyMs_[DUTY_BUCKETS];
  uint8_t  dutyIdx_;
  uint32_t dutyStart_;
};

void saveMesh(MeshCtx& c) {
  memcpy(c.echo_, echoNodes, sizeof(echoNodes)); c.echoCount_ = echoCount;
  c.nextEcho_ = loraNextEchoMs;
  memcpy(c.mail_, mailbox, sizeof(mailbox));
  c.msgRelayed_ = pendingMsgRelayed; c.courierFor_ = pendingCourierFor;
  c.echoSent_ = loraEchoesSent; c.echoRecv_ = loraEchoesRecv;
  c.mailDeliv_ = loraMailDelivered; c.mailHanded_ = loraMailHandedOff;
  c.mailExp_ = loraMailExpired;
  strncpy(c.pendingMsg_,  pendingMsg.c_str(),     sizeof(c.pendingMsg_) - 1);
  strncpy(c.pendingFrom_, pendingMsgFrom.c_str(), sizeof(c.pendingFrom_) - 1);
  c.pendingMsg_[sizeof(c.pendingMsg_) - 1] = '\0';
  c.pendingFrom_[sizeof(c.pendingFrom_) - 1] = '\0';
  c.beaconEnabled_ = loraBeaconEnabled; c.nextBeacon_ = loraNextBeaconMs;
  c.fastUntil_ = loraFastUntilMs; c.bootBurst_ = loraBootBurst;
  memcpy(c.dutyMs_, dutyBucketMs, sizeof(dutyBucketMs));
  c.dutyIdx_ = dutyBucketIdx; c.dutyStart_ = dutyBucketStart;
}

void loadMesh(MeshCtx& c) {
  memcpy(echoNodes, c.echo_, sizeof(echoNodes)); echoCount = c.echoCount_;
  loraNextEchoMs = c.nextEcho_;
  memcpy(mailbox, c.mail_, sizeof(mailbox));
  pendingMsgRelayed = c.msgRelayed_; pendingCourierFor = c.courierFor_;
  loraEchoesSent = c.echoSent_; loraEchoesRecv = c.echoRecv_;
  loraMailDelivered = c.mailDeliv_; loraMailHandedOff = c.mailHanded_;
  loraMailExpired = c.mailExp_;
  pendingMsg     = String(c.pendingMsg_);
  pendingMsgFrom = String(c.pendingFrom_);
  loraBeaconEnabled = c.beaconEnabled_; loraNextBeaconMs = c.nextBeacon_;
  loraFastUntilMs = c.fastUntil_; loraBootBurst = c.bootBurst_;
  memcpy(dutyBucketMs, c.dutyMs_, sizeof(dutyBucketMs));
  dutyBucketIdx = c.dutyIdx_; dutyBucketStart = c.dutyStart_;
}

void initMesh(MeshCtx& c) {
  memset(c.echo_, 0, sizeof(c.echo_)); c.echoCount_ = 0; c.nextEcho_ = 0;
  memset(c.mail_, 0, sizeof(c.mail_));
  c.msgRelayed_ = false; c.courierFor_ = 0;
  c.echoSent_ = c.echoRecv_ = c.mailDeliv_ = c.mailHanded_ = c.mailExp_ = 0;
  c.pendingMsg_[0] = c.pendingFrom_[0] = '\0';
  c.beaconEnabled_ = false; c.nextBeacon_ = 0; c.fastUntil_ = 0; c.bootBurst_ = 0;
  memset(c.dutyMs_, 0, sizeof(c.dutyMs_)); c.dutyIdx_ = 0; c.dutyStart_ = 0;
}
