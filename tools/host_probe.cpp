// host_probe --- report the host ABI facts the wire decoder depends on.
//
// The feed decoder reinterprets raw packet bytes in place (zero-copy) rather
// than deserialising field by field. That is only safe if the host satisfies a
// specific set of assumptions: 8-bit bytes, fixed integer widths, a known byte
// order, and structs that can be laid out with no implicit padding. This tool
// prints those properties and exits non-zero if any of them is violated, so the
// build can fail loudly on an unsupported target instead of silently decoding
// garbage.
//
//   build/bin/host_probe            # report + verify
//   build/bin/host_probe --quiet    # verify only, no output

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

// A statically allocated object, used below to show where the .data segment
// sits relative to the stack and the heap.
int g_static_object = 0;

// ---------------------------------------------------------------------------
// Wire-layout demonstration.
//
// A naturally aligned struct is NOT a faithful description of a byte stream:
// the compiler inserts padding so each member starts at an address that is a
// multiple of its own size. `packed` removes that padding. Every struct that
// overlays real packet bytes must be packed, or the field offsets will not
// match the protocol specification.
// ---------------------------------------------------------------------------
struct NaturalLayout {
    uint8_t  kind;
    uint32_t timestamp;
    uint16_t venue;
};

struct __attribute__((packed)) PackedLayout {
    uint8_t  kind;
    uint32_t timestamp;
    uint16_t venue;
};

void print_type_table() {
    std::printf("host type model\n");
    std::printf("  %-12s %5s %5s\n", "type", "size", "align");
    std::printf("  %-12s %5zu %5zu\n", "char", sizeof(char), alignof(char));
    std::printf("  %-12s %5zu %5zu\n", "short", sizeof(short), alignof(short));
    std::printf("  %-12s %5zu %5zu\n", "int", sizeof(int), alignof(int));
    std::printf("  %-12s %5zu %5zu\n", "long", sizeof(long), alignof(long));
    std::printf("  %-12s %5zu %5zu\n", "double", sizeof(double), alignof(double));
    std::printf("  %-12s %5zu %5zu   (an address, regardless of pointee)\n",
                "void*", sizeof(void*), alignof(void*));
    std::printf("  %-12s %5zu %5zu\n", "size_t", sizeof(size_t), alignof(size_t));
    std::printf("\n");
}

void print_byte_order() {
    // 0x12345678 is four distinct bytes. Which one lands at the lowest address
    // tells us the host byte order. Market data feeds (ITCH, MoldUDP64, and
    // most exchange protocols) are big-endian, so on a little-endian host every
    // multi-byte field needs a byte swap on decode.
    const uint32_t probe = 0x12345678u;
    uint8_t bytes[sizeof(probe)];
    std::memcpy(bytes, &probe, sizeof(probe));

    std::printf("byte order\n");
    std::printf("  value 0x%08X occupies bytes:", probe);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        std::printf(" [%zu]=0x%02X", i, bytes[i]);
    }
    std::printf("\n");

    const bool little = (std::endian::native == std::endian::little);
    std::printf("  std::endian::native = %s\n", little ? "little" : "big");
    std::printf("  wire format is big-endian -> decode %s byte-swap every"
                " multi-byte field\n\n",
                little ? "MUST" : "need not");
}

void print_struct_layout() {
    std::printf("struct layout (why wire structs must be packed)\n");
    std::printf("  %-14s size=%2zu  offsets: kind=%zu timestamp=%zu venue=%zu\n",
                "natural", sizeof(NaturalLayout), offsetof(NaturalLayout, kind),
                offsetof(NaturalLayout, timestamp), offsetof(NaturalLayout, venue));
    std::printf("  %-14s size=%2zu  offsets: kind=%zu timestamp=%zu venue=%zu\n",
                "packed", sizeof(PackedLayout), offsetof(PackedLayout, kind),
                offsetof(PackedLayout, timestamp), offsetof(PackedLayout, venue));
    std::printf("  -> the natural layout inserts %zu bytes of padding the wire"
                " format does not have\n\n",
                sizeof(NaturalLayout) - sizeof(PackedLayout));
}

void print_address_map() {
    // Four objects with four different storage classes. Their addresses reveal
    // the coarse structure of the process address space, which is the map we
    // will later carve a device BAR and a DMA region out of.
    int         on_stack = 0;
    static int  in_static_storage = 0;
    int* const  on_heap = new int(0);

    std::printf("process address map (one object per region)\n");
    std::printf("  %-22s %p\n", "code (this function)",
                reinterpret_cast<void*>(&print_address_map));
    std::printf("  %-22s %p\n", "static storage", static_cast<void*>(&in_static_storage));
    std::printf("  %-22s %p\n", "global object", static_cast<void*>(&g_static_object));
    std::printf("  %-22s %p\n", "heap allocation", static_cast<void*>(on_heap));
    std::printf("  %-22s %p\n", "stack frame", static_cast<void*>(&on_stack));
    std::printf("  -> distinct regions of one flat 64-bit address space;"
                " an address is just a number\n\n");

    delete on_heap;
}

// Returns the number of violated assumptions.
int verify_assumptions(bool quiet) {
    struct Check {
        const char* name;
        bool        ok;
    };

    const Check checks[] = {
        {"CHAR_BIT is 8", sizeof(uint8_t) == 1},
        {"uint16_t is exactly 2 bytes", sizeof(uint16_t) == 2},
        {"uint32_t is exactly 4 bytes", sizeof(uint32_t) == 4},
        {"uint64_t is exactly 8 bytes", sizeof(uint64_t) == 8},
        {"pointers are 64-bit", sizeof(void*) == 8},
        {"byte order is little-endian", std::endian::native == std::endian::little},
        {"packed structs carry no padding", sizeof(PackedLayout) == 7},
    };

    int failures = 0;
    if (!quiet) std::printf("assumption checks\n");
    for (const Check& c : checks) {
        if (!c.ok) ++failures;
        if (!quiet) std::printf("  [%s] %s\n", c.ok ? "pass" : "FAIL", c.name);
    }
    if (!quiet) {
        std::printf("\n%d of %zu checks failed\n", failures,
                    sizeof(checks) / sizeof(checks[0]));
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--quiet") quiet = true;
    }

    if (!quiet) {
        std::printf("host abi probe\n\n");
        print_type_table();
        print_byte_order();
        print_struct_layout();
        print_address_map();
    }

    return verify_assumptions(quiet) == 0 ? 0 : 1;
}
