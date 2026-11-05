#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "transport.h"

static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#pragma region TcpServerTransport
TcpServerTransport::~TcpServerTransport() {
    close();
}

bool TcpServerTransport::open() {
    if ((m_listenFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    if (bind(m_listenFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        fprintf(stderr, "bind to %u failed: %s\n", m_port, strerror(errno));
        return false;
    }

    if (listen(m_listenFd, 1) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        return false;
    }

    fprintf(stderr, "waiting for a head unit to connect on port %u\n", m_port);

    m_connFd = accept(m_listenFd, nullptr, nullptr);
    if (m_connFd < 0) {
        fprintf(stderr, "accept failed: %s\n", strerror(errno));
        return false;
    }

    setNonBlocking(m_connFd);
    fprintf(stderr, "head unit connected\n");

    return true;
}

ssize_t TcpServerTransport::read(uint8_t* buffer, size_t len) {
    if (m_connFd < 0) {
        return 0;
    }

    ssize_t readLen = recv(m_connFd, buffer, len, MSG_DONTWAIT);
    if (readLen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }

    return readLen;
}

bool TcpServerTransport::writeAll(const uint8_t* data, size_t len) {
    if (m_connFd < 0) {
        return false;
    }

    while (len > 0) {
        ssize_t written = send(m_connFd, data, len, MSG_NOSIGNAL);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd; pfd.fd = m_connFd; pfd.events = POLLOUT; pfd.revents = 0;
                poll(&pfd, 1, 5000);
                continue;
            }

            fprintf(stderr, "send failed: %s\n", strerror(errno));
            return false;
        }

        data += written;
        len -= (size_t)written;
    }

    return true;
}

void TcpServerTransport::close() {
    if (m_connFd >= 0) {
        ::close(m_connFd);
        m_connFd = -1;
    }

    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}
#pragma endregion TcpServerTransport

#pragma region UsbAccessoryTransport
UsbAccessoryTransport::~UsbAccessoryTransport() {
    close();
}

bool UsbAccessoryTransport::open() {
    // The node only exists once the gadget is enabled and the head unit started
    // the accessory session, so wait for it instead of failing.
    while (true) {
        struct stat st;
        if (stat(m_path.c_str(), &st) == 0) {
            break;
        }

        sleep(1);
    }

    if ((m_fd = ::open(m_path.c_str(), O_RDWR)) < 0) {
        fprintf(stderr, "opening %s failed: %s\n", m_path.c_str(), strerror(errno));
        return false;
    }

    fprintf(stderr, "opened %s\n", m_path.c_str());
    return true;
}

ssize_t UsbAccessoryTransport::read(uint8_t* buffer, size_t len) {
    if (m_fd < 0) {
        return 0;
    }

    ssize_t readLen = ::read(m_fd, buffer, len);
    if (readLen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENODEV)) {
        return -1;
    }

    return readLen;
}

bool UsbAccessoryTransport::writeAll(const uint8_t* data, size_t len) {
    if (m_fd < 0) {
        return false;
    }

    while (len > 0) {
        ssize_t written = ::write(m_fd, data, len);

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd; pfd.fd = m_fd; pfd.events = POLLOUT; pfd.revents = 0;
                poll(&pfd, 1, 5000);
                continue;
            }

            fprintf(stderr, "write to %s failed: %s\n", m_path.c_str(), strerror(errno));
            return false;
        }

        data += written;
        len -= (size_t)written;
    }

    return true;
}

void UsbAccessoryTransport::close() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}
#pragma endregion UsbAccessoryTransport
