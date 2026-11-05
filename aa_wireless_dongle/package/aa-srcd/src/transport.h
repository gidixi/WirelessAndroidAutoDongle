#pragma once

#include <cstdint>
#include <string>

// Byte stream carrying Android Auto frames: either the USB accessory gadget node
// (production) or a TCP socket (bench/testing against a software head unit).
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool open() = 0;
    // > 0 bytes read, 0 end of stream, -1 nothing available right now.
    virtual ssize_t read(uint8_t* buffer, size_t len) = 0;
    virtual bool writeAll(const uint8_t* data, size_t len) = 0;
    virtual void close() = 0;
    virtual int fd() const = 0;
};

// Development/bench transport: listen on a TCP port, accept one client.
class TcpServerTransport: public Transport {
public:
    explicit TcpServerTransport(uint16_t port): m_port(port) {}
    ~TcpServerTransport() override;

    bool open() override;
    ssize_t read(uint8_t* buffer, size_t len) override;
    bool writeAll(const uint8_t* data, size_t len) override;
    void close() override;
    int fd() const override { return m_connFd; }

private:
    uint16_t m_port;
    int m_listenFd = -1;
    int m_connFd = -1;
};

// Production transport: /dev/usb_accessory exposed by the f_accessory gadget
// function once the head unit starts the accessory session.
class UsbAccessoryTransport: public Transport {
public:
    explicit UsbAccessoryTransport(std::string path = "/dev/usb_accessory"): m_path(std::move(path)) {}
    ~UsbAccessoryTransport() override;

    // Waits until the accessory node shows up, then opens it.
    bool open() override;
    ssize_t read(uint8_t* buffer, size_t len) override;
    bool writeAll(const uint8_t* data, size_t len) override;
    void close() override;
    int fd() const override { return m_fd; }

private:
    std::string m_path;
    int m_fd = -1;
};
