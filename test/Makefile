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

.PHONY: all run asan portal clean
all: run portal

run: $(BINS)
	@for b in $(BINS); do echo "── $$b ──"; ./$$b || exit 1; echo; done

asan: CXXFLAGS += -g -fsanitize=address,undefined
asan: clean $(BINS)
	@for b in $(BINS); do echo "── $$b (asan) ──"; ./$$b || exit 1; echo; done

portal:
	@echo "── portal ──"
	@python3 portal/extract.py
	@if command -v node >/dev/null 2>&1; then \
	  node --check portal/portal.js && echo "portal.js parses OK"; \
	  cat portal/shim.js portal/portal.js portal/driver.js > portal/.run.js; \
	  node portal/.run.js || exit 1; \
	else echo "node not found — skipping portal render tests"; fi

test_link: test_link.cpp ../cypher32_lora.h ../cypher32_packets.h ../cypher32_crypto.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_twonode: test_twonode.cpp ../cypher32_lora.h ../cypher32_packets.h ../cypher32_crypto.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(BINS) portal/portal.html portal/portal.js portal/.run.js
