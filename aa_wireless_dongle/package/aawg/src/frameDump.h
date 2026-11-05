#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Records the Android Auto frames seen by the proxy, one file pair per direction.
//
// Message payloads are encrypted end to end between phone and head unit, so this
// does not try to decode them. Frame headers (channel id, flags, sizes) are always
// plaintext, and the raw payloads are kept on disk: that is enough to learn how a
// specific head unit behaves (which channels it opens, in which order, message
// sizes and timings) and to inspect the pre-TLS part of the session, which is
// plaintext (version exchange, TLS ClientHello / ServerHello, certificates).
//
// Output, in the configured directory:
//   capture-from-phone.bin     exact byte stream as read from the phone
//   capture-from-phone.idx.csv one line per parsed frame, offsets refer to the .bin
//   capture-from-headunit.*    same for the head unit side (USB accessory)
class FrameRecorder {
public:
    enum class Direction {
        FROM_PHONE,     // read from the phone over TCP
        FROM_HEADUNIT,  // read from the head unit over the USB accessory channel
    };

    FrameRecorder(Direction direction, const std::string& directory);
    ~FrameRecorder();

    FrameRecorder(const FrameRecorder&) = delete;
    FrameRecorder& operator=(const FrameRecorder&) = delete;

    // Append bytes exactly as they came off the wire. Frames may be split across
    // calls, or carry more than one frame; both are handled.
    void append(const unsigned char* data, size_t len);

private:
    static constexpr size_t HEADER_LEN_SHORT = 4;
    static constexpr size_t HEADER_LEN_FRAGMENTED = 8;
    static constexpr size_t MAX_PENDING = 128 * 1024;

    void openFiles();
    void parsePending();
    void logFrame(uint64_t offset, const unsigned char* header, size_t headerLen, uint32_t totalMsgSize);
    uint64_t elapsedMs() const;

    Direction m_direction;
    std::string m_directory;
    const char* m_name = nullptr;

    FILE* m_streamFile = nullptr;
    FILE* m_indexFile = nullptr;

    std::vector<unsigned char> m_pending;
    uint64_t m_pendingOffset = 0;  // absolute stream offset of m_pending[0]
    uint64_t m_startMs = 0;
};
