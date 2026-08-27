# Host-side tests for Cypher32.
#
#   make        — build and run everything
#   make asan   — link-layer tests under AddressSanitizer + UBSan
#   make portal — portal render tests only
#
# The C++ tests compile the real cypher32_lora.h against Arduino/RadioLib
# stubs. The portal tests extract the real HTML blob from cypher32_portal.h and
# run its render() against a minimal DOM shim.
#
# None of this substitutes for the T0.4 bench test or the field test protocol
# in ROADMAP.md — it exercises logic, not radios.

CXX      ?= g++
CXXFLAGS := -std=c++17 -I stub -Wall -Wno-format
BINS     := test_link test_twonode

.PHONY: all run asan portal lint qr sketch shots clean
all: lint sketch run portal qr

run: $(BINS)
	@for b in $(BINS); do echo "── $$b ──"; ./$$b || exit 1; echo; done

asan: CXXFLAGS += -g -fsanitize=address,undefined
asan: clean $(BINS)
	@for b in $(BINS); do echo "── $$b (asan) ──"; ./$$b || exit 1; echo; done

lint:
	@echo "── sketch lint ──"
	@python3 lint_sketch.py

portal:
	@echo "── portal ──"
	@python3 portal/extract.py
	@if command -v node >/dev/null 2>&1; then \
	  node --check portal/portal.js && echo "portal.js parses OK"; \
	  cat portal/shim.js portal/portal.js portal/driver.js > portal/.run.js; \
	  node portal/.run.js || exit 1; \
	else echo "node not found — skipping portal render tests"; fi

qr: test_qr test_qr_mask render_eink
	@echo "── qr ──"
	@python3 test_qr.py
	@./render_eink out >/dev/null && python3 test_qr_panel.py

# Compiles cypher32.ino against the host stubs. There is no ESP32 toolchain
# here, so this is the only thing that will tell you the sketch builds before
# the Arduino IDE does.
sketch: render_eink
	@echo "── sketch build ──"
	@mkdir -p out && ./render_eink out

# Regenerate every image in docs/img. Not part of `all`: it needs Chromium and
# Pillow, and it rewrites files that are committed.
shots: render_eink
	@mkdir -p out ../docs/img
	@./render_eink out
	@python3 pbm2png.py out ../docs/img
	@python3 portal/extract.py >/dev/null
	@NODE_PATH=$${NODE_PATH:-/opt/node22/lib/node_modules} node shoot_portal.js ../docs/img

render_eink: render_eink.cpp ../cypher32.ino ../cypher32_lora.h ../cypher32_packets.h \
             ../cypher32_portal.h ../cypher32_qr.h ../cypher32_crypto.h \
             stub/heltec-eink-modules.h stub/WiFi.h stub/WebServer.h stub/Preferences.h
	$(CXX) -std=c++17 -I stub -w -o $@ $<

test_qr: test_qr.cpp ../cypher32_qr.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_qr_mask: test_qr_mask.cpp ../cypher32_qr.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_link: test_link.cpp ../cypher32_lora.h ../cypher32_packets.h ../cypher32_crypto.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_twonode: test_twonode.cpp ../cypher32_lora.h ../cypher32_packets.h ../cypher32_crypto.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(BINS) test_qr test_qr_mask render_eink portal/portal.html portal/portal.js portal/.run.js
	rm -rf out
