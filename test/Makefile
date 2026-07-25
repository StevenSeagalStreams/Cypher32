# Host-side tests for the Cypher32 link layer.
#
#   make        — build and run everything
#   make asan   — same, under AddressSanitizer
#
# These compile the real cypher32_lora.h against Arduino/RadioLib stubs so the
# protocol logic can be exercised without hardware. They do NOT substitute for
# the T0.4 bench test or the field test protocol in ROADMAP.md.

CXX      ?= g++
CXXFLAGS := -std=c++17 -I stub -Wall -Wno-format
BINS     := test_link test_twonode

.PHONY: all run asan clean
all: run

run: $(BINS)
	@for b in $(BINS); do echo "── $$b ──"; ./$$b || exit 1; echo; done

asan: CXXFLAGS += -g -fsanitize=address,undefined
asan: clean $(BINS)
	@for b in $(BINS); do echo "── $$b (asan) ──"; ./$$b || exit 1; echo; done

test_link: test_link.cpp ../cypher32_lora.h ../cypher32_packets.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_twonode: test_twonode.cpp ../cypher32_lora.h ../cypher32_packets.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(BINS)
