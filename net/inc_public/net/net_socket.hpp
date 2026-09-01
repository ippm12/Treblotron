/**
 * net_socket.hpp
 *
 * Minimal blocking TCP wrapper over Winsock / BSD sockets. Deliberately tiny:
 * the dart protocol is one connection, one request/response cadence, and a
 * handful of message types, so there is nothing here to justify pulling in an
 * async networking library alongside the seven submodules the project already
 * carries.
 *
 * Winsock startup/teardown is reference-counted internally, so callers just
 * construct and destroy these objects.
 */

#ifndef NET_SOCKET_HPP
#define NET_SOCKET_HPP

#include "common_inc.hpp"

#include <cstddef>
#include <cstdint>
#include <string>


/** Platform socket handle. Winsock's SOCKET is unsigned; POSIX uses int. */
#ifdef _WIN32
using NetSocketHandle = uintptr_t;
constexpr NetSocketHandle NET_INVALID_SOCKET = static_cast<NetSocketHandle>(~0ull);
#else
using NetSocketHandle = int;
constexpr NetSocketHandle NET_INVALID_SOCKET = -1;
#endif


/** A connected TCP stream. Move-only; closes on destruction. */
class NetSocket
{
    public:
        NetSocket() = default;
        explicit NetSocket(NetSocketHandle handle);
        ~NetSocket();

        NetSocket(NetSocket&& other) noexcept;
        NetSocket& operator=(NetSocket&& other) noexcept;
        NetSocket(const NetSocket&) = delete;
        NetSocket& operator=(const NetSocket&) = delete;

        /**
         * Connect to host:port. `timeoutMs` bounds the connect attempt only;
         * subsequent reads block indefinitely unless setReadTimeout is used.
         * Returns STATUS_OK on success.
         */
        Status connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

        /** Read exactly `bytes` into `dst`. False on EOF or error. */
        bool readExact(void* dst, size_t bytes);

        /** Write all of `src`. False on error. */
        bool writeAll(const void* src, size_t bytes);

        /**
         * Disable Nagle. Worth doing on both ends: the protocol alternates
         * small control messages with large JPEG payloads, and coalescing the
         * control messages adds latency straight onto the detection path.
         */
        void setNoDelay(bool enable);

        /** 0 disables the timeout. A timed-out read reports failure. */
        void setReadTimeout(int ms);

        /**
         * Size the kernel socket buffers.
         *
         * Matters when the two ends deliberately overlap: the peer pushes the
         * next frame set while this end is busy and not reading, so the whole
         * set has to fit in the receive buffer or the sender stalls part-way
         * and the overlap is lost. Windows defaults to 64 KB, well under a
         * three-camera JPEG set.
         *
         * Note this disables Linux's receive-buffer auto-tuning, so pass
         * something generous rather than something merely sufficient.
         */
        void setBufferSizes(int receiveBytes, int sendBytes);

        /**
         * True when the socket has data (or an EOF) waiting, within the
         * timeout. Lets a caller stay responsive to other work while waiting on
         * a peer, instead of parking inside a blocking read — which is how the
         * server keeps answering new connections while a session is idle.
         */
        bool waitReadable(int timeoutMs) const;

        bool isOpen() const { return m_handle != NET_INVALID_SOCKET; }
        void close();

        /** Peer address as "ip:port", for logging. Empty if unavailable. */
        std::string peerName() const;

        /**
         * Peer address without the port. The port changes on every reconnect,
         * so this is what identifies "the same machine coming back".
         */
        std::string peerAddress() const;

    private:
        NetSocketHandle m_handle = NET_INVALID_SOCKET;
};


/** A listening TCP socket. */
class NetListener
{
    public:
        NetListener() = default;
        ~NetListener();

        NetListener(const NetListener&) = delete;
        NetListener& operator=(const NetListener&) = delete;

        /**
         * Bind to `port` on all interfaces and start listening. SO_REUSEADDR
         * is set so a restarted server doesn't trip over TIME_WAIT.
         */
        Status listen(uint16_t port, int backlog = 4);

        /**
         * Wait for a client to connect.
         *
         *   timeoutMs < 0   block until one arrives
         *   timeoutMs == 0  poll: return immediately, open socket or not
         *   timeoutMs > 0   wait up to that long
         *
         * A closed socket means nothing was pending, which lets an accept loop
         * poll a shutdown flag, and lets a busy session check for gate-crashers
         * without stalling.
         */
        NetSocket accept(int timeoutMs = -1);

        bool isOpen() const { return m_handle != NET_INVALID_SOCKET; }
        void close();

    private:
        NetSocketHandle m_handle = NET_INVALID_SOCKET;
};

#endif // NET_SOCKET_HPP
