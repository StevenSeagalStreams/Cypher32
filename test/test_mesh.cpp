// Three-node mesh: echo presence and courier mail.
//
// The two-node harness cannot express this feature at all — its air is
// point-to-point, so "A cannot hear C" is not a state it can represent, and
// that state IS the feature. This one carries a reachability matrix and
// delivers every broadcast to everyone who can actually hear the sender.
//
// The standing topology is a chain:
//
//      A <---> B <---> C <---> D      each hears only its neighbours
//
// which is the shape the whole design exists for. Four links rather than three
// because some rules only have anything to bite on when a carrier has a
// further carrier available to it — a three-node chain cannot express "B is
// holding mail for someone B cannot reach, and C could take it".
#include <Arduino.h>
#include <cstdio>
#include <vector>

uint32_t   g_millis   = 1000;
uint32_t   g_rngState = 31337;
SerialStub Serial;
ESPStub    ESP;

uint32_t myChipID32 = 0;
String   myFaction  = "BLACK";
int      myLevel    = 1;
int      skillBrute = 10, skillStealth = 4, skillFirewall = 6;

#include "../cypher32_lora.h"
#include "nodectx.h"

// ── the air, with a topology ──
#define NODES 4
struct Dev { NodeCtx link; MeshCtx mesh; };
Dev      dev[NODES];
uint32_t devId[NODES] = { 0xA0000001u, 0xB0000002u, 0xC0000003u, 0xD0000004u };
bool     canHear[NODES][NODES];          // canHear[listener][speaker]

struct InFlight { std::vector<uint8_t> data; uint32_t arriveMs; int dest; };
std::vector<InFlight> air;

void chain() {                            // neighbours only, nobody further
  for (int i = 0; i < NODES; i++)
    for (int j = 0; j < NODES; j++)
      canHear[i][j] = (i != j) && (i - j == 1 || j - i == 1);
}
void fullyConnected() {
  for (int i = 0; i < NODES; i++)
    for (int j = 0; j < NODES; j++) canHear[i][j] = (i != j);
}

void loadDev(int i)  { load(dev[i].link);  loadMesh(dev[i].mesh);  }
void saveDev(int i)  { save(dev[i].link);  saveMesh(dev[i].mesh);  }

void initDev(int i, const char* fac) {
  initCtx(dev[i].link, devId[i], fac, 10, 6);
  initMesh(dev[i].mesh);
  loadDev(i);
  loraBeaconEnabled = true;
  loraNextEchoMs = 0;
  saveDev(i);
}

void stepDev(int i, uint32_t step) {
  (void)step;
  loadDev(i);
  if (radioState == RS_TX) {
    if ((uint32_t)(millis() - txStartMs) >= 50) loraDioFlag = true;
  } else {
    for (size_t k = 0; k < air.size(); k++) {
      if (air[k].dest != i) continue;
      if ((int32_t)(millis() - air[k].arriveMs) < 0) continue;
      radio.rxBuf = air[k].data;
      loraDioFlag = true;
      air.erase(air.begin() + k);
      break;
    }
  }
  loraTick();
  // A transmission reaches every device that can hear this one — which is what
  // makes a broadcast a broadcast, and what the two-node harness could not do.
  for (auto& f : radio.sent)
    for (int d = 0; d < NODES; d++)
      if (d != i && canHear[d][i]) air.push_back({f.data, millis() + 50, d});
  radio.sent.clear();
  saveDev(i);
}

void run(uint32_t ms) {
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    advance(10);
    for (int i = 0; i < NODES; i++) stepDev(i, 10);
  }
}

int checks = 0, failures = 0;
void CHECK(bool c, const char* what) {
  checks++;
  if (!c) { failures++; printf("  FAIL: %s\n", what); }
}

int main() {
  printf("Cypher32 mesh: echo presence and courier mail\n\n");
  const uint32_t A = devId[0], B = devId[1], C = devId[2], D = devId[3];

  // ── echo carries presence exactly one hop, and no further ──
  {
    printf("echo reaches two hops\n");
    for (int i = 0; i < NODES; i++) initDev(i, i == 1 ? "WHITE" : "BLACK");
    air.clear(); chain(); g_millis = 1000;

    run(ECHO_INTERVAL_MS + ECHO_JITTER_MS + 20000);

    loadDev(0);
    CHECK(findNode(B) != nullptr, "A hears B directly");
    CHECK(findNode(C) == nullptr, "A does NOT hear C directly");
    CHECK(findEcho(C) != nullptr, "but an echo tells A that C exists");
    EchoNode* e = findEcho(C);
    CHECK(e && e->via == B,       "and names B as the one who can reach them");
    CHECK(echoCarrierFor(C) == B, "so B is offered as the carrier for C");
    saveDev(0);

    loadDev(2);
    CHECK(findEcho(A) != nullptr, "and it works in the other direction too");
    saveDev(2);
  }

  // ── an echo is a rumour, not a contact ──
  // If this ever stops holding, a player can farm XP off someone they have
  // never been near, and every gate in the game is written as findNode().
  {
    printf("an echo is not a contact\n");
    loadDev(0);
    CHECK(findNode(C) == nullptr,
          "an echoed node is absent from knownNodes, so every action gate refuses it");
    int inCensus = 0;
    for (int i = 0; i < knownCount; i++) if (knownNodes[i].chip_id == C) inCensus++;
    CHECK(inCensus == 0, "and is counted by nothing that iterates the node table");
    EchoNode* e = findEcho(C);
    CHECK(e != nullptr, "the echo record exists");
    // The struct has nowhere to put a signal reading even if someone tried.
    CHECK(sizeof(EchoNode) == 3 * sizeof(uint32_t),
          "and carries only who, via whom, and when — no RSSI to misreport");
    saveDev(0);
  }

  // ── a rumour is dropped the moment the truth arrives ──
  {
    printf("direct contact supersedes the echo\n");
    fullyConnected();                       // C walks into A's range
    run(60000);
    loadDev(0);
    CHECK(findNode(C) != nullptr, "A now hears C for itself");
    CHECK(findEcho(C) == nullptr,
          "and the echo is dropped — a worse copy of something true");
    saveDev(0);
    chain();                                // and back out again
  }

  // ── the feature: a message to someone you cannot reach ──
  {
    printf("courier mail across the gap\n");
    for (int i = 0; i < NODES; i++) initDev(i, i == 1 ? "WHITE" : "BLACK");
    air.clear(); chain(); g_millis = 1000;
    run(ECHO_INTERVAL_MS + ECHO_JITTER_MS + 20000);   // let the echoes settle

    loadDev(0);
    CHECK(echoCarrierFor(C) == B, "A knows B can reach C");
    CHECK(mailQueue(C, A, "meet at the bar", false), "A writes to C");
    CHECK(mailPending() == 1, "and it waits in the outbox");
    saveDev(0);

    run(60000);

    loadDev(0);
    CHECK(mailPending() == 0, "A's outbox empties once B takes the message");
    CHECK(loraMailHandedOff == 1, "recorded as handed off, not as delivered");
    saveDev(0);

    loadDev(2);
    CHECK(pendingMsg == String("meet at the bar"),
          "and C receives it, having never been in range of A");
    CHECK(pendingMsgFrom == chipIdStr(A),
          "credited to the author, not to the courier who carried it");
    CHECK(pendingMsgRelayed, "and marked as having arrived by courier");
    saveDev(2);

    loadDev(1);
    CHECK(mailPending() == 0, "B's bag is empty again once it lands");
    CHECK(loraMailDelivered == 1, "B recorded the delivery");
    saveDev(1);
  }

  // ── two hops is the whole budget ──
  {
    printf("a carried message is never carried twice\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;

    run(ECHO_INTERVAL_MS + ECHO_JITTER_MS + 20000);   // echoes settle

    loadDev(1);
    // B is carrying somebody else's message for D. B cannot hear D, and an
    // echo says C can — so there IS an onward carrier to be tempted by.
    CHECK(findNode(D) == nullptr,      "B cannot hear D");
    CHECK(echoCarrierFor(D) == C,      "but B knows C can reach D");
    CHECK(mailQueue(D, A, "onward", true), "B is holding carried mail for D");
    saveDev(1);
    run(90000);

    loadDev(1);
    CHECK(mailPending() == 1,
          "B does not hand it on: two hops is the whole budget");
    CHECK(loraMailHandedOff == 0, "so nothing was passed to a second carrier");
    saveDev(1);
    loadDev(2);
    CHECK(mailPending() == 0, "and C never took it");
    saveDev(2);
  }

  // The sender-side rule above is only half of it. A peer that ignored it, or
  // an older build, would still put a twice-carried frame on the air — so the
  // receiver refuses one too. That guard cannot be reached by playing the
  // scenario out, because our own sender never creates it; inject it instead.
  {
    printf("a carried frame is refused on arrival\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;

    loadDev(0);
    PktMail p;
    fillHdr(&p.hdr, PKT_MAIL, myChipID32);
    p.hdr.from_id = B; p.hdr.seq = 201;
    p.final_id = 0xD0000004u; p.origin_id = C; p.carried = 1;
    strncpy(p.text, "onward", MAIL_TEXT_MAX); p.text[MAIL_TEXT_MAX] = '\0';
    loraHandlePacket((uint8_t*)&p, sizeof(p));
    CHECK(mailPending() == 0,
          "a frame already carried once is refused, whoever sent it");

    // ...and the same frame with the flag clear IS taken, so the check above
    // is testing the flag and not some unrelated reason to drop the packet.
    p.hdr.seq = 202; p.carried = 0;
    loraHandlePacket((uint8_t*)&p, sizeof(p));
    CHECK(mailPending() == 1, "while a first-hop frame is accepted for carriage");
    CHECK(mailCarriedCount() == 1, "and is marked as somebody else's post");
    saveDev(0);
  }

  // ── a carrier that has walked away is not a carrier ──
  {
    printf("a stale carrier is not offered\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;
    run(ECHO_INTERVAL_MS + ECHO_JITTER_MS + 20000);

    loadDev(0);
    CHECK(echoCarrierFor(C) == B, "B is the carrier while B is live");
    KnownNode* b = findNode(B);
    CHECK(b != nullptr, "B is in the node table");
    // B stops transmitting. The echo it left behind is still inside its TTL,
    // so the rumour outlives the bridge that carried it.
    b->last_seen_ms = millis() - NODE_ACTIVE_MS - 1000;
    CHECK(findEcho(C) != nullptr, "the echo record has not expired yet");
    CHECK(echoCarrierFor(C) == 0,
          "but a carrier we can no longer hear is not offered as one");
    saveDev(0);
  }

  // pruneEchoes() would eventually delete a rumour about someone we can hear,
  // but "eventually" is up to NODE_PRUNE_MS away and the echo table has twelve
  // slots. An entry that should never have been written can evict a real one
  // in the meantime, so echoRecord refuses it at the door.
  {
    printf("a rumour is never recorded about someone we can hear\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); fullyConnected(); g_millis = 1000;
    run(40000);                                    // everyone hears everyone

    loadDev(0);
    CHECK(findNode(B) != nullptr && findNode(C) != nullptr,
          "A hears both B and C directly");
    int before = echoCount;
    PktEcho p;
    fillHdr(&p.hdr, PKT_ECHO, 0);
    p.hdr.from_id = B; p.hdr.seq = 191;
    memset(p.peers, 0, sizeof(p.peers));
    // Named by B, so it is not caught by the sender-is-the-subject guard, and
    // A can hear C for itself — which is the case the door check is for.
    p.count = 1; p.peers[0] = C;
    loraHandlePacket((uint8_t*)&p, sizeof(p));
    CHECK(echoCount == before && findEcho(C) == nullptr,
          "no echo row is created for a node already in the table");

    // Nor about ourselves: B names A in every echo it sends, and A must not
    // end up as a rumour in its own table.
    p.hdr.seq = 192; p.peers[0] = A;
    loraHandlePacket((uint8_t*)&p, sizeof(p));
    CHECK(findEcho(A) == nullptr, "and never about ourselves");

    // Nor about the sender, who is by definition a direct contact.
    p.hdr.seq = 193; p.peers[0] = B;
    loraHandlePacket((uint8_t*)&p, sizeof(p));
    CHECK(findEcho(B) == nullptr, "nor about the neighbour who sent the echo");
    saveDev(0);
  }

  // ── mail for an unreachable stranger must not accumulate forever ──
  {
    printf("undeliverable mail expires\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;

    loadDev(0);
    CHECK(mailQueue(0xDEADBEEFu, A, "into the void", false), "queued for a stranger");
    saveDev(0);

    // MAIL_MAX_TRIES never fires: with no carrier there is nobody to try.
    // It is the TTL that has to catch this one.
    for (int i = 0; i < 40; i++) { advance(60000); for (int k = 0; k < NODES; k++) stepDev(k, 10); }

    loadDev(0);
    CHECK(mailPending() == 0, "and it is dropped once the TTL passes");
    CHECK(loraMailExpired == 1, "counted as expired, not as delivered");
    saveDev(0);
  }

  // ── the courier bag cannot be filled by other people ──
  {
    printf("other people's post cannot crowd out your own\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;

    loadDev(1);
    // The cap is enforced where carriage is accepted, so push frames at it
    // rather than calling mailQueue() directly and walking straight past it.
    for (int i = 0; i < MAIL_SLOTS + 4; i++) {
      PktMail p;
      fillHdr(&p.hdr, PKT_MAIL, myChipID32);
      p.hdr.from_id = A; p.hdr.seq = (uint8_t)(100 + i);
      p.final_id = 0xE0000000u + i; p.origin_id = A; p.carried = 0;
      snprintf(p.text, sizeof p.text, "m%d", i);
      loraHandlePacket((uint8_t*)&p, sizeof(p));
    }
    CHECK(mailCarriedCount() <= MAIL_SLOTS / 2,
          "other people's post can never fill more than half the bag");
    CHECK(mailPending() < MAIL_SLOTS,
          "so there is always room left for the player's own messages");

    // Prove the reserved half is usable: our own mail still queues.
    CHECK(mailQueue(C, myChipID32, "mine", false),
          "and the player can still write one");
    saveDev(1);
  }

  // ── the message log remembers who carried what ──
  {
    printf("message log\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;
    loadDev(0);
    msgLogCount = 0; msgLogNext = 0;

    // A direct message and a carried one, through the real RX path.
    PktMsg m;
    fillHdr(&m.hdr, PKT_MSG, myChipID32);
    m.hdr.from_id = B; m.hdr.seq = 40;
    strncpy(m.text, "in person", 32); m.text[32] = '\0';
    loraHandlePacket((uint8_t*)&m, sizeof(m));

    PktMail p;
    fillHdr(&p.hdr, PKT_MAIL, myChipID32);
    p.hdr.from_id = B; p.hdr.seq = 41;          // B is the courier
    p.final_id = myChipID32; p.origin_id = D;   // D wrote it
    p.carried = 0;
    strncpy(p.text, "from far away", MAIL_TEXT_MAX); p.text[MAIL_TEXT_MAX] = '\0';
    loraHandlePacket((uint8_t*)&p, sizeof(p));

    CHECK(msgLogCount == 2, "both messages are logged");
    CHECK(msgLogAt(0) != nullptr && msgLogAt(1) != nullptr, "and both are readable");
    const MsgLogEntry* newest = msgLogAt(0);
    CHECK(newest && newest->from == D, "the carried one is credited to its author");
    CHECK(newest && newest->via == B,  "and names the player who carried it");
    const MsgLogEntry* older = msgLogAt(1);
    CHECK(older && older->from == B,   "the direct one is from whoever sent it");
    CHECK(older && older->via == 0,    "and has no carrier");
    CHECK(lastMsgVia == B, "the e-ink page can name the carrier too");



    // Mail the author delivers in person is not "carried" — the sender and the
    // author are the same device, and labelling it "via themselves" would be
    // both wrong and confusing on the page that reports the route.
    PktMail own;
    fillHdr(&own.hdr, PKT_MAIL, myChipID32);
    own.hdr.from_id = C; own.hdr.seq = 42;
    own.final_id = myChipID32; own.origin_id = C;   // author IS the sender
    own.carried = 0;
    strncpy(own.text, "delivered myself", MAIL_TEXT_MAX); own.text[MAIL_TEXT_MAX] = '\0';
    loraHandlePacket((uint8_t*)&own, sizeof(own));
    CHECK(msgLogAt(0) && msgLogAt(0)->via == 0,
          "mail handed over by its own author records no carrier");
    CHECK(lastMsgVia == 0, "and the page shows no route for it");

    // Two from the same sender must both survive — the per-node inbox held
    // one message per node and simply overwrote the first.
    for (int i = 0; i < 2; i++) {
      PktMsg q; fillHdr(&q.hdr, PKT_MSG, myChipID32);
      q.hdr.from_id = B; q.hdr.seq = (uint8_t)(50 + i);
      snprintf(q.text, sizeof q.text, "same sender %d", i);
      loraHandlePacket((uint8_t*)&q, sizeof(q));
    }
    CHECK(msgLogCount == 5, "two messages from one sender are both kept");
    // Guarded: a regression that stops logging leaves these null, and a test
    // that segfaults reports nothing at all rather than naming what broke.
    CHECK(msgLogAt(0) && String(msgLogAt(0)->text) == "same sender 1", "newest first");
    CHECK(msgLogAt(1) && String(msgLogAt(1)->text) == "same sender 0",
          "and the older one survives");

    // It is a ring: the eleventh must push out the first, not overflow.
    for (int i = 0; i < MSG_LOG_SIZE + 5; i++) {
      PktMsg q; fillHdr(&q.hdr, PKT_MSG, myChipID32);
      q.hdr.from_id = C; q.hdr.seq = (uint8_t)(80 + i);
      snprintf(q.text, sizeof q.text, "flood %d", i);
      loraHandlePacket((uint8_t*)&q, sizeof(q));
    }
    CHECK(msgLogCount == MSG_LOG_SIZE, "the log holds exactly ten");
    CHECK(msgLogAt(0) && String(msgLogAt(0)->text) == "flood " + String(MSG_LOG_SIZE + 4),
          "the newest is kept");
    CHECK(msgLogAt(MSG_LOG_SIZE) == nullptr, "and reading past the end is refused");
    saveDev(0);
  }

  // ── delivering somebody's post is an introduction ──
  // A route that reads "via UNKNOWN-0002" tells you nothing about who to
  // thank. Carrying mail identifies the carrier the same way writing to you
  // or attacking you does — free codename, no odds bonus.
  //
  // In its own block on purpose: the courier must not have introduced itself
  // some other way first. The earlier version of this test used a node that
  // had already sent a direct message, so it passed with the rule deleted.
  {
    printf("the courier introduces itself\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); chain(); g_millis = 1000;
    loadDev(0);
    knownCount = 0; memset(knownNodes, 0, sizeof(knownNodes));
    msgLogCount = 0; msgLogNext = 0;

    PktMail p;
    fillHdr(&p.hdr, PKT_MAIL, myChipID32);
    p.hdr.from_id = C; p.hdr.seq = 90;          // C carries it, and has said
    p.final_id = myChipID32; p.origin_id = D;   // nothing to us before now
    p.carried = 0;
    strncpy(p.text, "hand delivered", MAIL_TEXT_MAX); p.text[MAIL_TEXT_MAX] = '\0';
    loraHandlePacket((uint8_t*)&p, sizeof(p));

    KnownNode* carrier = findNode(C);
    CHECK(carrier != nullptr, "the courier is a direct contact — we heard them");
    CHECK(carrier && reconKnows(carrier, RECON_T_NAME),
          "and is identified by having made the delivery");
    CHECK(carrier && carrier->recon_score == 0,
          "but earns no recon score from it — a name is not an odds bonus");
    CHECK(carrier && !reconKnows(carrier, RECON_T_FACTION),
          "and nothing beyond a name is handed over");

    // The AUTHOR is not introduced by this. They were not here.
    KnownNode* author = findNode(D);
    CHECK(author == nullptr || !reconKnows(author, RECON_T_NAME),
          "the author, who was never in range, gets no such favour");
    saveDev(0);
  }

  // ── airtime: the reason this design exists ──
  {
    printf("airtime\n");
    for (int i = 0; i < NODES; i++) initDev(i, "BLACK");
    air.clear(); fullyConnected(); g_millis = 1000;
    loadDev(0); dutyBucketStart = 0; for (int i = 0; i < DUTY_BUCKETS; i++) dutyBucketMs[i] = 0; saveDev(0);

    run(600000);                                   // ten minutes of idling

    loadDev(0);
    // dutyCyclePct() is airtime as a fraction of a whole hour, and we have
    // only simulated ten minutes of one, so it reads six times low. Comparing
    // that number against the cap directly would pass no matter what the
    // firmware did. Extrapolate to the hour it is measured against.
    float pct = dutyCyclePct() * 6.0f;
    printf("  four nodes idle, extrapolated to the hour: %.3f %% duty\n", pct);
    CHECK(pct > 0.05f,          "the devices are actually transmitting");
    CHECK(pct < DUTY_LIMIT_PCT, "steady-state duty stays under the firmware cap");
    // Against the band this profile actually transmits in, not against g1 —
    // LONG and EPIC sit in g3, where the legal ceiling is ten times higher.
    CHECK(pct < LORA_DUTY_LEGAL_PCT,
          "and under the legal ceiling for the band the profile uses");
    // The limit that bites first is the shared channel, not the regulator.
    printf("  aggregate at 20 nodes would be %.1f %% of the channel\n", pct * 20.0f);
    // The echo is a fixed cost per node per interval, so it does not grow with
    // the size of the room. Flooding would have: 20 nodes x 51.5 ms per beacon
    // interval is 3.43 %, which is why none of this rebroadcasts anything.
    CHECK(loraEchoesSent >= 3 && loraEchoesSent <= 7,
          "and echoes go out on their own slow clock, roughly one per interval");
    saveDev(0);
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
