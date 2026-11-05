#pragma once

#include <cstdint>
#include <deque>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <vector>

#include "transport.h"

struct Message {
    uint8_t channel = 0;
    bool encrypted = false;
    bool control = false;
    std::vector<uint8_t> payload;

    // Convenience for the two byte message id that prefixes every payload.
    uint16_t messageId() const {
        return payload.size() >= 2 ? (uint16_t)((payload[0] << 8) | payload[1]) : 0;
    }
};

// Application layer TLS 1.2: the handshake bytes travel inside Android Auto
// control messages and the record layer runs over in-memory BIOs, exactly like
// the head unit implementations do. We are the server (the phone role).
class Cryptor {
public:
    ~Cryptor();

    bool initServer(const char* certificatePem, const char* privateKeyPem);
    bool isReady() const { return m_ready; }

    // Feed handshake bytes received from the peer; appended output must be sent back.
    bool handleHandshake(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out);
    bool encrypt(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out);
    bool decrypt(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out);

    const std::string& peerSubject() const { return m_peerSubject; }

private:
    void drainNetwork(std::vector<uint8_t>& out);
    bool feedNetwork(const uint8_t* in, size_t inLen);

    SSL_CTX* m_context = nullptr;
    SSL* m_ssl = nullptr;
    BIO* m_internalBio = nullptr;
    BIO* m_networkBio = nullptr;
    bool m_ready = false;
    std::string m_peerSubject;
};

// Frame level reader/writer: reassembles fragmented messages, splits large ones,
// and applies the cryptor based on the encryption bit of each frame.
class Messenger {
public:
    Messenger(Transport& transport, Cryptor& cryptor): m_transport(transport), m_cryptor(cryptor) {}

    // Some head units reject large frames; keep this tunable while benching.
    void setMaxFramePayload(size_t maxFramePayload) { m_maxFramePayload = maxFramePayload < 64 ? 64 : maxFramePayload; }

    // Reads whatever is available from the transport and queues complete messages.
    // Returns false when the peer closed the stream or the data is malformed.
    bool pump();
    bool nextMessage(Message& out);
    bool send(Message message);
    bool peerClosed() const { return m_peerClosed; }

private:
    bool readAvailable();
    bool consumeFrames();
    bool decodePayload(std::vector<uint8_t>& payload, bool encrypted);

    Transport& m_transport;
    Cryptor& m_cryptor;

    std::vector<uint8_t> m_rx;
    std::vector<uint8_t> m_fragment;
    uint8_t m_fragmentChannel = 0;
    bool m_fragmentControl = false;
    bool m_fragmentEncrypted = false;
    bool m_fragmentActive = false;

    size_t m_maxFramePayload = 32768;

    std::deque<Message> m_queue;
    bool m_peerClosed = false;
};
