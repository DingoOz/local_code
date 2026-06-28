// MarkerFilter must strip leaked special-token markers even when they are split
// across streamed chunks, and never drop legitimate text.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "../src/stream_filter.hpp"

using namespace lc;

// Drive the filter one character at a time (worst case for split markers).
static std::string stream(const std::string& in) {
    MarkerFilter f;
    std::string out;
    for (char c : in) out += f.feed(std::string(1, c));
    out += f.flush();
    return out;
}

int main() {
    // The whole channel header (marker + name) is dropped; the answer remains.
    assert(stream("<|channel>thought\n<channel|>Hello there") == "Hello there");
    assert(stream("<|channel>analysis<|message|>real answer") == "real answer");
    assert(stream("plain text, no markers") == "plain text, no markers");

    // A '<' that is not a marker must survive (e.g. code / comparisons).
    assert(stream("#include <iostream>\nif (a < b) ok") ==
           "#include <iostream>\nif (a < b) ok");

    // Marker delivered as a single chunk.
    MarkerFilter f;
    std::string out = f.feed("A<|message|>B");
    out += f.flush();
    assert(out == "AB");

    // A multi-byte UTF-8 character ("—" = E2 80 94) split across chunks must be
    // reassembled, never emitted in partial pieces (which corrupts terminals).
    assert(stream("No problem \xE2\x80\x94 done") == "No problem \xE2\x80\x94 done");
    {
        MarkerFilter g;
        std::string r = g.feed("ab\xE2\x80");  // ends mid-character: hold the tail
        assert(r == "ab");                       // partial bytes withheld
        r += g.feed("\x94" "cd");               // completes "—"
        r += g.flush();
        assert(r == "ab\xE2\x80\x94" "cd");
    }
    // The helper itself: count of incomplete trailing bytes.
    assert(utf8_incomplete_suffix_len("ab\xE2\x80", 4) == 2);  // 2 of 3 bytes
    assert(utf8_incomplete_suffix_len("ab\xE2\x80\x94", 5) == 0);  // complete
    assert(utf8_incomplete_suffix_len("plain", 5) == 0);

    std::cout << "FILTER TESTS PASS\n";
    return 0;
}
