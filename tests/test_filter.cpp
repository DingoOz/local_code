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

    std::cout << "FILTER TESTS PASS\n";
    return 0;
}
