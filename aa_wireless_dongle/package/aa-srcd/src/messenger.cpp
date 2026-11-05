#include <cstdio>
#include <cstring>

#include "messenger.h"

namespace {
    constexpr unsigned char FRAME_TYPE_FIRST = 1 << 0;
    constexpr unsigned char FRAME_TYPE_MASK = (1 << 0) | (1 << 1);
    constexpr unsigned char MESSAGE_TYPE_CONTROL = 1 << 2;
    constexpr unsigned char ENCRYPTION_ENCRYPTED = 1 << 3;

    constexpr size_t kChunkSize = 16384;

    // Head units present self-signed certificates of their own. Google's phone
    // implementation accepts any certificate except the ones whose subject
    // contains "CarService" or the embedded Google Automotive Link CA; we accept
    // everything and just log what we got.
    int verifyAlwaysAccept(int, X509_STORE_CTX*) {
        return 1;
    }
}

Cryptor::~Cryptor() {
    // SSL_free releases the BIOs attached with SSL_set_bio.
    if (m_ssl) {
        SSL_free(m_ssl);
    }

    if (m_context) {
        SSL_CTX_free(m_context);
    }
}

bool Cryptor::initServer(const char* certificatePem, const char* privateKeyPem) {
    m_context = SSL_CTX_new(TLS_server_method());
    if (!m_context) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        return false;
    }

    // Android Auto uses TLS 1.2 only; do not let OpenSSL 3 negotiate 1.3.
    SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(m_context, TLS1_2_VERSION);
    SSL_CTX_set_options(m_context, SSL_OP_NO_TICKET | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verifyAlwaysAccept);

    BIO* certificateBio = BIO_new_mem_buf(certificatePem, -1);
    X509* certificate = PEM_read_bio_X509(certificateBio, nullptr, nullptr, nullptr);
    BIO_free(certificateBio);

    if (!certificate || SSL_CTX_use_certificate(m_context, certificate) != 1) {
        fprintf(stderr, "cannot load our certificate\n");
        if (certificate) {
            X509_free(certificate);
        }
        return false;
    }
    X509_free(certificate);

    BIO* keyBio = BIO_new_mem_buf(privateKeyPem, -1);
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    BIO_free(keyBio);

    if (!privateKey || SSL_CTX_use_PrivateKey(m_context, privateKey) != 1 || SSL_CTX_check_private_key(m_context) != 1) {
        fprintf(stderr, "cannot load our private key\n");
        if (privateKey) {
            EVP_PKEY_free(privateKey);
        }
        return false;
    }
    EVP_PKEY_free(privateKey);

    m_ssl = SSL_new(m_context);
    if (!m_ssl) {
        fprintf(stderr, "SSL_new failed\n");
        return false;
    }

    if (BIO_new_bio_pair(&m_internalBio, 0, &m_networkBio, 0) != 1) {
        fprintf(stderr, "BIO_new_bio_pair failed\n");
        return false;
    }

    // Both sides of the TLS connection go through the internal end of the BIO
    // pair; the network end is what we read/write to move bytes around.
    SSL_set_bio(m_ssl, m_internalBio, m_internalBio);
    SSL_set_mode(m_ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_set_accept_state(m_ssl);

    return true;
}

void Cryptor::drainNetwork(std::vector<uint8_t>& out) {
    unsigned char buffer[kChunkSize];
    int readLen;

    while ((readLen = BIO_read(m_networkBio, buffer, sizeof(buffer))) > 0) {
        out.insert(out.end(), buffer, buffer + readLen);
    }
}

bool Cryptor::feedNetwork(const uint8_t* in, size_t inLen) {
    size_t offset = 0;
    int stalledRounds = 0;

    while (offset < inLen) {
        int written = BIO_write(m_networkBio, in + offset, (int)(inLen - offset));

        if (written > 0) {
            offset += (size_t)written;
            stalledRounds = 0;
            continue;
        }

        if (!BIO_should_retry(m_networkBio)) {
            return false;
        }

        // The record layer has not consumed what we wrote yet; give it a chance
        // by pushing whatever application data is pending out before retrying.
        unsigned char appBuffer[kChunkSize];
        if (SSL_read(m_ssl, appBuffer, sizeof(appBuffer)) <= 0 && ++stalledRounds > 50) {
            return false;
        }
    }

    return true;
}

bool Cryptor::handleHandshake(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    if (getenv("AA_SRCD_DEBUG_TLS") && inLen > 0) {
        fprintf(stderr, "TLS in: %zu bytes, first=%02x/%02x/%02x state=%s\n", inLen,
            in[0], in[1], in[2], SSL_state_string(m_ssl));
    }
    if (inLen > 0 && !feedNetwork(in, inLen)) {
        fprintf(stderr, "TLS: cannot feed handshake bytes\n");
        return false;
    }

    for (int round = 0; round < 32; round++) {
        int result = SSL_do_handshake(m_ssl);
        drainNetwork(out);

        if (result == 1) {
            m_ready = true;

            X509* peerCertificate = SSL_get_peer_certificate(m_ssl);
            if (peerCertificate) {
                char* subject = X509_NAME_oneline(X509_get_subject_name(peerCertificate), nullptr, 0);
                if (subject) {
                    m_peerSubject = subject;
                    OPENSSL_free(subject);
                }
                X509_free(peerCertificate);
            }

            fprintf(stderr, "TLS established with head unit [%s]\n", m_peerSubject.c_str());
            return true;
        }

        int error = SSL_get_error(m_ssl, result);
        if (error == SSL_ERROR_WANT_READ) {
            return true;
        }

        fprintf(stderr, "TLS handshake failed: %d\n", error);
        ERR_print_errors_fp(stderr);
        return false;
    }

    return false;
}

bool Cryptor::encrypt(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    size_t offset = 0;

    while (offset < inLen) {
        size_t chunk = inLen - offset < kChunkSize ? inLen - offset : kChunkSize;
        int written = SSL_write(m_ssl, in + offset, (int)chunk);

        if (written <= 0) {
            int error = SSL_get_error(m_ssl, written);
            drainNetwork(out);

            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                fprintf(stderr, "TLS encrypt failed: %d\n", error);
                return false;
            }

            continue;
        }

        offset += (size_t)written;
        drainNetwork(out);
    }

    return true;
}

bool Cryptor::decrypt(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    if (!feedNetwork(in, inLen)) {
        fprintf(stderr, "TLS: cannot feed encrypted bytes\n");
        return false;
    }

    unsigned char buffer[kChunkSize];

    while (true) {
        int readLen = SSL_read(m_ssl, buffer, sizeof(buffer));

        if (readLen > 0) {
            out.insert(out.end(), buffer, buffer + readLen);
            continue;
        }

        int error = SSL_get_error(m_ssl, readLen);

        if (error == SSL_ERROR_NONE || error == SSL_ERROR_ZERO_RETURN || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            return true;
        }

        fprintf(stderr, "TLS decrypt failed: %d\n", error);
        return false;
    }
}

#pragma region Messenger
bool Messenger::pump() {
    if (!readAvailable()) {
        return false;
    }

    return consumeFrames();
}

bool Messenger::readAvailable() {
    unsigned char buffer[kChunkSize];

    while (true) {
        ssize_t readLen = m_transport.read(buffer, sizeof(buffer));

        if (readLen == 0) {
            m_peerClosed = true;
            return false;
        }

        if (readLen < 0) {
            return true;
        }

        m_rx.insert(m_rx.end(), buffer, buffer + readLen);
    }
}

bool Messenger::consumeFrames() {
    while (m_rx.size() >= 4) {
        const uint8_t* frame = m_rx.data();
        bool fragmented = (frame[1] & FRAME_TYPE_MASK) == FRAME_TYPE_FIRST;
        size_t headerLen = fragmented ? 8 : 4;

        if (m_rx.size() < headerLen) {
            break;
        }

        uint16_t payloadLen = (uint16_t)((frame[2] << 8) | frame[3]);
        if (m_rx.size() < headerLen + payloadLen) {
            break;
        }

        uint8_t channel = frame[0];
        bool control = (frame[1] & MESSAGE_TYPE_CONTROL) != 0;
        bool encrypted = (frame[1] & ENCRYPTION_ENCRYPTED) != 0;
        const uint8_t* payload = frame + headerLen;

        Message message;

        if (fragmented) {
            m_fragmentActive = true;
            m_fragmentChannel = channel;
            m_fragmentControl = control;
            m_fragmentEncrypted = encrypted;
            m_fragment.assign(payload, payload + payloadLen);
        }
        else if (!m_fragmentActive) {
            message.channel = channel;
            message.control = control;
            message.encrypted = encrypted;
            message.payload.assign(payload, payload + payloadLen);
        }
        else {
            m_fragment.insert(m_fragment.end(), payload, payload + payloadLen);

            bool last = (frame[1] & FRAME_TYPE_MASK) == (1 << 1);
            if (last) {
                message.channel = m_fragmentChannel;
                message.control = m_fragmentControl;
                message.encrypted = m_fragmentEncrypted;
                message.payload.swap(m_fragment);
                m_fragmentActive = false;
            }
        }

        size_t used = headerLen + payloadLen;
        m_rx.erase(m_rx.begin(), m_rx.begin() + used);

        if (!message.payload.empty()) {
            if (!decodePayload(message.payload, message.encrypted)) {
                return false;
            }

            m_queue.push_back(std::move(message));
        }
    }

    return true;
}

bool Messenger::decodePayload(std::vector<uint8_t>& payload, bool encrypted) {
    if (!encrypted || payload.empty()) {
        return true;
    }

    std::vector<uint8_t> plain;
    if (!m_cryptor.decrypt(payload.data(), payload.size(), plain)) {
        return false;
    }

    payload.swap(plain);
    return true;
}

bool Messenger::nextMessage(Message& out) {
    if (m_queue.empty()) {
        return false;
    }

    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

bool Messenger::send(Message message) {
    std::vector<uint8_t> wire;

    if (message.encrypted) {
        if (!m_cryptor.encrypt(message.payload.data(), message.payload.size(), wire)) {
            return false;
        }
    }
    else {
        wire = std::move(message.payload);
    }

    bool fragmented = wire.size() > m_maxFramePayload;
    size_t offset = 0;

    do {
        size_t remaining = wire.size() - offset;
        size_t chunk = remaining > m_maxFramePayload ? m_maxFramePayload : remaining;
        bool first = offset == 0;
        bool last = offset + chunk >= wire.size();

        unsigned char flags = (message.control ? MESSAGE_TYPE_CONTROL : 0) | (message.encrypted ? ENCRYPTION_ENCRYPTED : 0);
        if (!fragmented) {
            flags |= 0x03; // BULK
        }
        else if (first) {
            flags |= FRAME_TYPE_FIRST;
        }
        else if (last) {
            flags |= (1 << 1);
        }

        std::vector<uint8_t> frame;
        size_t headerLen = (fragmented && first) ? 8 : 4;
        frame.reserve(headerLen + chunk);
        frame.push_back(message.channel);
        frame.push_back(flags);
        frame.push_back((unsigned char)((chunk >> 8) & 0xff));
        frame.push_back((unsigned char)(chunk & 0xff));

        if (fragmented && first) {
            uint32_t total = (uint32_t)wire.size();
            frame.push_back((unsigned char)((total >> 24) & 0xff));
            frame.push_back((unsigned char)((total >> 16) & 0xff));
            frame.push_back((unsigned char)((total >> 8) & 0xff));
            frame.push_back((unsigned char)(total & 0xff));
        }

        frame.insert(frame.end(), wire.begin() + offset, wire.begin() + offset + chunk);

        if (!m_transport.writeAll(frame.data(), frame.size())) {
            return false;
        }

        offset += chunk;
    } while (offset < wire.size());

    return true;
}
#pragma endregion Messenger
