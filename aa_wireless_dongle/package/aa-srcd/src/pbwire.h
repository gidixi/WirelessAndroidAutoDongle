#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal protobuf wire format helpers.
//
// The Android Auto messages we exchange are small and we only need a handful of
// their fields, so instead of pulling protoc and libprotobuf into the image we
// encode/decode the wire format directly. Unknown fields are skipped, which is
// what makes this tolerant across head unit firmware versions.
namespace pb {

enum WireType {
    VARINT = 0,
    FIXED64 = 1,
    LENGTH_DELIMITED = 2,
    FIXED32 = 5,
};

struct Field {
    uint32_t number = 0;
    uint8_t wire = 0;
    uint64_t value = 0;      // VARINT, FIXED32, FIXED64
    std::string bytes;       // LENGTH_DELIMITED (also nested messages)
};

bool parse(const uint8_t* data, size_t len, std::vector<Field>& fields);

// Returns the first field with the given number, or nullptr.
const Field* find(const std::vector<Field>& fields, uint32_t number);
// Collects every field with the given number (repeated fields).
std::vector<const Field*> findAll(const std::vector<Field>& fields, uint32_t number);

class Writer {
public:
    void addVarint(uint32_t number, uint64_t value);
    void addInt(uint32_t number, int64_t value);
    void addString(uint32_t number, const std::string& value);
    void addMessage(uint32_t number, const Writer& nested);

    const std::string& data() const { return m_data; }
    size_t size() const { return m_data.size(); }

private:
    void appendTag(uint32_t number, uint8_t wireType);
    void appendVarint(uint64_t value);

    std::string m_data;
};

} // namespace pb
