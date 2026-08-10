/**
 * net_socket.cpp
 *
 * Winsock / BSD socket implementation. The only genuinely platform-divergent
 * parts are library startup, the close call, the error accessor, and the
 * timeout option's argument type — everything else is shared.
 */

#include "net/net_socket.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif


namespace
{
#ifdef _WIN32
    // Winsock needs explicit startup, and it is reference-counted by the OS —
    // but WSACleanup on the last reference tears down for the whole process,
    // so we start it once and never clean up. The process exiting does that.
    void ensureWinsock()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            WSADATA wsa;
            const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
            if(rc != 0)
            {
                LOG_ERROR(NET_LOG_ID, "WSAStartup failed: {}", rc);
            }
        });
    }

    int lastError()            { return WSAGetLastError(); }
    void closeHandle(NetSocketHandle h) { closesocket(static_cast<SOCKET>(h)); }
    constexpr int kWouldBlock  = WSAEWOULDBLOCK;
    constexpr int kInProgress  = WSAEWOULDBLOCK;
    using SockLen  = int;
    using OptValue = const char*;
#else
    void ensureWinsock() {}
    int  lastError()           { return errno; }
    void closeHandle(NetSocketHandle h) { ::close(h); }
    constexpr int kWouldBlock  = EWOULDBLOCK;
    constexpr int kInProgress  = EINPROGRESS;
    using SockLen  = socklen_t;
    using OptValue = const void*;
#endif

    /** Raw handle as the type the platform's socket calls expect. */
    inline auto raw(NetSocketHandle h)
    {
#ifdef _WIN32
        return static_cast<SOCKET>(h);
#else
        return h;
#endif
    }

    void setBlocking(NetSocketHandle h, bool blocking)
    {
#ifdef _WIN32
        u_long mode = blocking ? 0 : 1;
        ioctlsocket(raw(h), FIONBIO, &mode);
#else
        int flags = fcntl(h, F_GETFL, 0);
        if(flags < 0) return;
        fcntl(h, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
    }
}


// ============================================================================
// NetSocket
// ============================================================================

NetSocket::NetSocket(NetSocketHandle handle) : m_handle(handle) {}


NetSocket::~NetSocket() { close(); }


NetSocket::NetSocket(NetSocket&& other) noexcept : m_handle(other.m_handle)
{
    other.m_handle = NET_INVALID_SOCKET;
}


NetSocket& NetSocket::operator=(NetSocket&& other) noexcept
{
    if(this != &other)
    {
        close();
        m_handle = other.m_handle;
        other.m_handle = NET_INVALID_SOCKET;
    }
    return *this;
}


void NetSocket::close()
{
    if(m_handle != NET_INVALID_SOCKET)
    {
        closeHandle(m_handle);
        m_handle = NET_INVALID_SOCKET;
    }
}


Status NetSocket::connect(const std::string& host, uint16_t port, int timeoutMs)
{
    ensureWinsock();
    close();

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &results);
    if(rc != 0 || !results)
    {
        LOG_ERROR(NET_LOG_ID, "getaddrinfo({}:{}) failed: {}", host, port, rc);
        return STATUS_ERROR_GENERIC;
    }

    Status status = STATUS_ERROR_GENERIC;
    for(addrinfo* ai = results; ai; ai = ai->ai_next)
    {
        const NetSocketHandle h =
            static_cast<NetSocketHandle>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if(h == NET_INVALID_SOCKET) continue;

        // Non-blocking connect + select so a dead host fails in `timeoutMs`
        // rather than the platform's multi-second default.
        setBlocking(h, false);
        const int cr = ::connect(raw(h), ai->ai_addr, static_cast<SockLen>(ai->ai_addrlen));
        bool connected = (cr == 0);

        if(!connected && (lastError() == kInProgress || lastError() == kWouldBlock))
        {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(raw(h), &wr);
            timeval tv{};
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            if(::select(static_cast<int>(raw(h)) + 1, nullptr, &wr, nullptr, &tv) > 0)
            {
                int soErr = 0;
                SockLen len = sizeof(soErr);
                if(::getsockopt(raw(h), SOL_SOCKET, SO_ERROR,
                                reinterpret_cast<char*>(&soErr), &len) == 0 && soErr == 0)
                {
                    connected = true;
                }
            }
        }

        setBlocking(h, true);

        if(connected)
        {
            m_handle = h;
            status = STATUS_OK;
            break;
        }
        closeHandle(h);
    }

    freeaddrinfo(results);
    return status;
}


bool NetSocket::readExact(void* dst, size_t bytes)
{
    if(m_handle == NET_INVALID_SOCKET) return false;

    auto* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while(got < bytes)
    {
        const int n = ::recv(raw(m_handle), reinterpret_cast<char*>(p + got),
                             static_cast<int>(bytes - got), 0);
        if(n > 0)      { got += static_cast<size_t>(n); continue; }
        if(n == 0)     return false;  // orderly shutdown by peer
        return false;                 // error or timeout
    }
    return true;
}


bool NetSocket::writeAll(const void* src, size_t bytes)
{
    if(m_handle == NET_INVALID_SOCKET) return false;

    const auto* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while(sent < bytes)
    {
        const int n = ::send(raw(m_handle), reinterpret_cast<const char*>(p + sent),
                             static_cast<int>(bytes - sent), 0);
        if(n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}


void NetSocket::setNoDelay(bool enable)
{
    if(m_handle == NET_INVALID_SOCKET) return;
    const int flag = enable ? 1 : 0;
    ::setsockopt(raw(m_handle), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<OptValue>(&flag), sizeof(flag));
}


void NetSocket::setReadTimeout(int ms)
{
    if(m_handle == NET_INVALID_SOCKET) return;
#ifdef _WIN32
    // Winsock takes a DWORD of milliseconds; POSIX takes a struct timeval.
    DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(raw(m_handle), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<OptValue>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(raw(m_handle), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<OptValue>(&tv), sizeof(tv));
#endif
}


namespace
{
    /** Shared by peerName/peerAddress; empty on any failure. */
    bool peerEndpoint(NetSocketHandle handle, std::string& ip, uint16_t& port)
    {
        if(handle == NET_INVALID_SOCKET) return false;

        sockaddr_in addr{};
        SockLen len = sizeof(addr);
        if(::getpeername(raw(handle), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            return false;
        }

        char buf[INET_ADDRSTRLEN] = {};
        if(!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return false;
        ip   = buf;
        port = ntohs(addr.sin_port);
        return true;
    }
}


void NetSocket::setBufferSizes(int receiveBytes, int sendBytes)
{
    if(m_handle == NET_INVALID_SOCKET) return;

    if(receiveBytes > 0)
    {
        ::setsockopt(raw(m_handle), SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<OptValue>(&receiveBytes), sizeof(receiveBytes));
    }
    if(sendBytes > 0)
    {
        ::setsockopt(raw(m_handle), SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<OptValue>(&sendBytes), sizeof(sendBytes));
    }
}


bool NetSocket::waitReadable(int timeoutMs) const
{
    if(m_handle == NET_INVALID_SOCKET) return false;

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(raw(m_handle), &rd);
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return ::select(static_cast<int>(raw(m_handle)) + 1, &rd, nullptr, nullptr, &tv) > 0;
}


std::string NetSocket::peerName() const
{
    std::string ip;
    uint16_t    port = 0;
    if(!peerEndpoint(m_handle, ip, port)) return {};
    return ip + ":" + std::to_string(port);
}


std::string NetSocket::peerAddress() const
{
    std::string ip;
    uint16_t    port = 0;
    if(!peerEndpoint(m_handle, ip, port)) return {};
    return ip;
}


// ============================================================================
// NetListener
// ============================================================================

NetListener::~NetListener() { close(); }


void NetListener::close()
{
    if(m_handle != NET_INVALID_SOCKET)
    {
        closeHandle(m_handle);
        m_handle = NET_INVALID_SOCKET;
    }
}


Status NetListener::listen(uint16_t port, int backlog)
{
    ensureWinsock();
    close();

    const NetSocketHandle h =
        static_cast<NetSocketHandle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if(h == NET_INVALID_SOCKET)
    {
        LOG_ERROR(NET_LOG_ID, "socket() failed: {}", lastError());
        return STATUS_ERROR_GENERIC;
    }

    const int reuse = 1;
    ::setsockopt(raw(h), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<OptValue>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if(::bind(raw(h), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        LOG_ERROR(NET_LOG_ID, "bind(port {}) failed: {}", port, lastError());
        closeHandle(h);
        return STATUS_ERROR_GENERIC;
    }

    if(::listen(raw(h), backlog) != 0)
    {
        LOG_ERROR(NET_LOG_ID, "listen(port {}) failed: {}", port, lastError());
        closeHandle(h);
        return STATUS_ERROR_GENERIC;
    }

    m_handle = h;
    LOG_INFO(NET_LOG_ID, "listening on port {}", port);
    return STATUS_OK;
}


NetSocket NetListener::accept(int timeoutMs)
{
    if(m_handle == NET_INVALID_SOCKET) return NetSocket();

    if(timeoutMs >= 0)
    {
        // select() first so an accept loop can wake up periodically and check
        // its shutdown flag instead of blocking forever on a quiet port. A
        // zero timeout makes this a pure poll, which is how a live session
        // notices a second client without stalling its own frame loop.
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(raw(m_handle), &rd);
        timeval tv{};
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if(::select(static_cast<int>(raw(m_handle)) + 1, &rd, nullptr, nullptr, &tv) <= 0)
        {
            return NetSocket();
        }
    }

    const NetSocketHandle client =
        static_cast<NetSocketHandle>(::accept(raw(m_handle), nullptr, nullptr));
    if(client == NET_INVALID_SOCKET) return NetSocket();

    return NetSocket(client);
}
