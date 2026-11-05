#include "pbwire.h"

namespace pb {

static bool readVarint(const uint8_t* data, size_t len, size_t& offset, uint64_t& value) {
    value = 0;
    int shift = 0;

    while (offset < len) {
        uint8_t byte = data[offset++];
        value |= (uint64_t)(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            return true;
        }

        shift += 7;
        if (shift > 63) {
            return false;
        }
    }

    return false;
}

bool parse(const uint8_t* data, size_t len, std::vector<Field>& fields) {
    size_t offset = 0;

    while (offset < len) {
        uint64_t tag = 0;
        if (!readVarint(data, len, offset, tag)) {
            return false;
        }

        Field field;
        field.number = (uint32_t)(tag >> 3);
        field.wire = (uint8_t)(tag & 0x07);

        switch (field.wire) {
            case VARINT:
                if (!readVarint(data, len, offset, field.value)) {
                    return false;
                }
                break;
            case FIXED64:
                if (len - offset < 8) {
                    return false;
                }
                for (int i = 0; i < 8; i++) {
                    field.value |= (uint64_t)data[offset + i] << (i * 8);
                }
                offset += 8;
                break;
            case FIXED32:
                if (len - offset < 4) {
                    return false;
                }
                for (int i = 0; i < 4; i++) {
                    field.value |= (uint64_t)data[offset + i] << (i * 8);
                }
                offset += 4;
                break;
            case LENGTH_DELIMITED: {
                uint64_t fieldLen = 0;
                if (!readVarint(data, len, offset, fieldLen) || fieldLen > len - offset) {
                    return false;
                }
                field.bytes.assign((const char*)data + offset, (size_t)fieldLen);
                offset += (size_t)fieldLen;
                break;
            }
            default:
                // Groups (start/end) are not used by Android Auto.
                return false;
        }

        fields.push_back(std::move(field));
    }

    return true;
}

const Field* find(const std::vector<Field>& fields, uint32_t number) {
    for (const Field& field: fields) {
        if (field.number == number) {
            return &field;
        }
    }

    return nullptr;
}

std::vector<const Field*> findAll(const std::vector<Field>& fields, uint32_t number) {
    std::vector<const Field*> result;

    for (const Field& field: fields) {
        if (field.number == number) {
            result.push_back(&field);
        }
    }

    return result;
}

void Writer::appendTag(uint32_t number, uint8_t wireType) {
    appendVarint(((uint64_t)number << 3) | wireType);
}

void Writer::appendVarint(uint64_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;

        m_data.push_back(value ? (byte | 0x80) : byte);
    } while (value);
}

void Writer::addVarint(uint32_t number, uint64_t value) {
    appendTag(number, VARINT);
    appendVarint(value);
}

void Writer::addInt(uint32_t number, int64_t value) {
    addVarint(number, (uint64_t)value);
}

void Writer::addString(uint32_t number, const std::string& value) {
    appendTag(number, LENGTH_DELIMITED);
    appendVarint(value.size());
    m_data.append(value);
}

void Writer::addMessage(uint32_t number, const Writer& nested) {
    appendTag(number, LENGTH_DELIMITED);
    appendVarint(nested.size());
    m_data.append(nested.data());
}

} // namespace pb
