/**
 * dart_protocol.cpp
 *
 * Encode/decode for the client↔server dart protocol. Each message is built
 * into one contiguous buffer and written with a single writeAll so a message
 * never interleaves with another writer's bytes.
 *
 * Decoding goes through a bounds-checked cursor: every read is validated
 * against the remaining payload, so a truncated or hostile message fails the
 * parse instead of walking off the buffer.
 */

#include "net/dart_protocol.hpp"

#include <cstring>


namespace
{
    // ------------------------------------------------------------------
    // Writer
    // ------------------------------------------------------------------
    struct Writer
    {
        std::vector<uint8_t> buf;

        void raw(const void* p, size_t n)
        {
            const auto* b = static_cast<const uint8_t*>(p);
            buf.insert(buf.end(), b, b + n);
        }

        void u8 (uint8_t  v) { raw(&v, sizeof(v)); }
        void u16(uint16_t v) { raw(&v, sizeof(v)); }
        void u32(uint32_t v) { raw(&v, sizeof(v)); }
        void u64(uint64_t v) { raw(&v, sizeof(v)); }
        void f32(float    v) { raw(&v, sizeof(v)); }

        void str(const std::string& s)
        {
            // Strings are bounded so a bad length can't request a huge alloc.
            const uint32_t n = static_cast<uint32_t>(
                s.size() > 65535 ? 65535 : s.size());
            u32(n);
            raw(s.data(), n);
        }

        void bytes(const std::vector<uint8_t>& v)
        {
            u32(static_cast<uint32_t>(v.size()));
            raw(v.data(), v.size());
        }
    };


    // ------------------------------------------------------------------
    // Reader — every accessor bounds-checks and latches a failure flag.
    // ------------------------------------------------------------------
    struct Reader
    {
        const uint8_t* p   = nullptr;
        size_t         n   = 0;
        size_t         pos = 0;
        bool           ok  = true;

        explicit Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}

        bool take(void* dst, size_t bytes)
        {
            if(!ok || pos + bytes > n) { ok = false; return false; }
            std::memcpy(dst, p + pos, bytes);
            pos += bytes;
            return true;
        }

        uint8_t  u8 () { uint8_t  v = 0; take(&v, sizeof(v)); return v; }
        uint16_t u16() { uint16_t v = 0; take(&v, sizeof(v)); return v; }
        uint32_t u32() { uint32_t v = 0; take(&v, sizeof(v)); return v; }
        uint64_t u64() { uint64_t v = 0; take(&v, sizeof(v)); return v; }
        float    f32() { float    v = 0; take(&v, sizeof(v)); return v; }

        std::string str()
        {
            const uint32_t len = u32();
            if(!ok || pos + len > n) { ok = false; return {}; }
            std::string s(reinterpret_cast<const char*>(p + pos), len);
            pos += len;
            return s;
        }

        std::vector<uint8_t> bytes()
        {
            const uint32_t len = u32();
            if(!ok || pos + len > n) { ok = false; return {}; }
            std::vector<uint8_t> v(p + pos, p + pos + len);
            pos += len;
            return v;
        }
    };


    /** Prepend the header and write the whole message in one go. */
    bool sendFramed(NetSocket& sock, DartMsg type, const Writer& w)
    {
        DartMsgHeader h;
        h.type         = static_cast<uint16_t>(type);
        h.payloadBytes = static_cast<uint32_t>(w.buf.size());

        std::vector<uint8_t> out;
        out.reserve(sizeof(h) + w.buf.size());
        const auto* hb = reinterpret_cast<const uint8_t*>(&h);
        out.insert(out.end(), hb, hb + sizeof(h));
        out.insert(out.end(), w.buf.begin(), w.buf.end());

        return sock.writeAll(out.data(), out.size());
    }
}


// ============================================================================
// Framing
// ============================================================================

bool dartRecvHeader(NetSocket& sock, DartMsgHeader& out)
{
    if(!sock.readExact(&out, sizeof(out))) return false;

    if(out.magic != DART_PROTOCOL_MAGIC)
    {
        LOG_ERROR(NET_LOG_ID, "protocol desync: magic 0x{:08x}, expected 0x{:08x}",
                  out.magic, DART_PROTOCOL_MAGIC);
        return false;
    }
    if(out.payloadBytes > DART_PROTOCOL_MAX_PAYLOAD)
    {
        LOG_ERROR(NET_LOG_ID, "payload of {} bytes exceeds the {} byte cap",
                  out.payloadBytes, DART_PROTOCOL_MAX_PAYLOAD);
        return false;
    }
    return true;
}


bool dartRecvPayload(NetSocket& sock, const DartMsgHeader& header,
                     std::vector<uint8_t>& out)
{
    out.resize(header.payloadBytes);
    if(header.payloadBytes == 0) return true;
    return sock.readExact(out.data(), out.size());
}


// ============================================================================
// Senders
// ============================================================================

bool dartSendHello(NetSocket& sock, const DartHello& msg)
{
    Writer w;
    w.u32(msg.protocolVersion);
    w.u32(msg.cameraCount);
    w.u32(msg.nativeWidth);
    w.u32(msg.nativeHeight);
    w.u8(msg.wantHeatmap ? 1 : 0);
    w.u32(static_cast<uint32_t>(msg.wirePointsXY.size()));
    for(const std::vector<float>& cam : msg.wirePointsXY)
    {
        w.u32(static_cast<uint32_t>(cam.size()));
        for(float v : cam) w.f32(v);
    }
    return sendFramed(sock, DartMsg::Hello, w);
}


bool dartSendHelloAck(NetSocket& sock, const DartHelloAck& msg)
{
    Writer w;
    w.u32(msg.protocolVersion);
    w.u32(static_cast<uint32_t>(msg.status));
    w.u32(msg.maxInFlight);
    w.str(msg.message);
    return sendFramed(sock, DartMsg::HelloAck, w);
}


bool dartSendInitProgress(NetSocket& sock, const DartInitProgress& msg)
{
    Writer w;
    w.f32(msg.progress);
    w.u8(static_cast<uint8_t>(msg.state));
    w.u64(msg.iteration);
    w.str(msg.status);
    return sendFramed(sock, DartMsg::InitProgress, w);
}


bool dartSendWantFrames(NetSocket& sock, uint32_t credits)
{
    Writer w;
    w.u32(credits);
    return sendFramed(sock, DartMsg::WantFrames, w);
}


bool dartSendFrames(NetSocket& sock, const DartFrames& msg)
{
    Writer w;
    w.u32(msg.sequence);
    w.u64(msg.captureTime);
    w.u32(static_cast<uint32_t>(msg.jpegs.size()));
    for(const std::vector<uint8_t>& j : msg.jpegs) w.bytes(j);
    return sendFramed(sock, DartMsg::Frames, w);
}


bool dartSendDetection(NetSocket& sock, const DartDetectionMsg& msg)
{
    Writer w;
    w.u32(msg.sequence);
    w.u8(msg.boardClear ? 1 : 0);
    w.u32(static_cast<uint32_t>(msg.darts.size()));
    for(const DartDetectionMsg::Dart& d : msg.darts)
    {
        w.f32(d.angle);
        w.f32(d.normalizedRadius);
    }
    w.str(msg.status);
    return sendFramed(sock, DartMsg::Detection, w);
}


bool dartSendHeatmap(NetSocket& sock, const DartHeatmapMsg& msg)
{
    Writer w;
    w.u32(msg.width);
    w.u32(msg.height);
    w.bytes(msg.values);
    return sendFramed(sock, DartMsg::Heatmap, w);
}


bool dartSendCaptureSaved(NetSocket& sock, const DartCaptureSaved& msg)
{
    Writer w;
    w.u8(msg.ok ? 1 : 0);
    w.str(msg.message);
    return sendFramed(sock, DartMsg::CaptureSaved, w);
}


bool dartSendSimple(NetSocket& sock, DartMsg type)
{
    Writer w;
    return sendFramed(sock, type, w);
}


// ============================================================================
// Parsers
// ============================================================================

bool dartParseHello(const std::vector<uint8_t>& payload, DartHello& out)
{
    Reader r(payload);
    out.protocolVersion = r.u32();
    out.cameraCount     = r.u32();
    out.nativeWidth     = r.u32();
    out.nativeHeight    = r.u32();
    out.wantHeatmap     = (r.u8() != 0);

    const uint32_t cams = r.u32();
    if(!r.ok || cams > 16) return false;

    out.wirePointsXY.clear();
    out.wirePointsXY.reserve(cams);
    for(uint32_t c = 0; c < cams; c++)
    {
        const uint32_t count = r.u32();
        // Two floats per point, and no rig has more than a few hundred points.
        if(!r.ok || count > 4096) return false;
        std::vector<float> pts;
        pts.reserve(count);
        for(uint32_t i = 0; i < count; i++) pts.push_back(r.f32());
        if(!r.ok) return false;
        out.wirePointsXY.push_back(std::move(pts));
    }
    return r.ok;
}


bool dartParseHelloAck(const std::vector<uint8_t>& payload, DartHelloAck& out)
{
    Reader r(payload);
    out.protocolVersion = r.u32();

    // An unknown status is treated as a rejection rather than optimistically
    // as an acceptance — a client must never proceed to send frames to a
    // server whose answer it did not understand.
    const uint32_t status = r.u32();
    out.status = (status <= static_cast<uint32_t>(DartHandshake::Busy))
               ? static_cast<DartHandshake>(status)
               : DartHandshake::Rejected;

    out.maxInFlight = r.u32();
    out.message     = r.str();
    return r.ok;
}


bool dartParseInitProgress(const std::vector<uint8_t>& payload, DartInitProgress& out)
{
    Reader r(payload);
    out.progress  = r.f32();
    const uint8_t state = r.u8();
    out.state     = (state <= static_cast<uint8_t>(DartInitState::Failed))
                  ? static_cast<DartInitState>(state)
                  : DartInitState::Failed;
    out.iteration = r.u64();
    out.status    = r.str();
    return r.ok;
}


bool dartParseWantFrames(const std::vector<uint8_t>& payload, uint32_t& credits)
{
    Reader r(payload);
    credits = r.u32();
    return r.ok;
}


bool dartParseFrames(const std::vector<uint8_t>& payload, DartFrames& out)
{
    Reader r(payload);
    out.sequence    = r.u32();
    out.captureTime = r.u64();

    const uint32_t cams = r.u32();
    if(!r.ok || cams > 16) return false;

    out.jpegs.clear();
    out.jpegs.reserve(cams);
    for(uint32_t c = 0; c < cams; c++)
    {
        out.jpegs.push_back(r.bytes());
        if(!r.ok) return false;
    }
    return r.ok;
}


bool dartParseDetection(const std::vector<uint8_t>& payload, DartDetectionMsg& out)
{
    Reader r(payload);
    out.sequence   = r.u32();
    out.boardClear = (r.u8() != 0);

    const uint32_t count = r.u32();
    if(!r.ok || count > 64) return false;

    out.darts.clear();
    out.darts.reserve(count);
    for(uint32_t i = 0; i < count; i++)
    {
        DartDetectionMsg::Dart d{};
        d.angle            = r.f32();
        d.normalizedRadius = r.f32();
        out.darts.push_back(d);
    }
    out.status = r.str();
    return r.ok;
}


bool dartParseCaptureSaved(const std::vector<uint8_t>& payload, DartCaptureSaved& out)
{
    Reader r(payload);
    out.ok      = (r.u8() != 0);
    out.message = r.str();
    return r.ok;
}


bool dartParseHeatmap(const std::vector<uint8_t>& payload, DartHeatmapMsg& out)
{
    Reader r(payload);
    out.width  = r.u32();
    out.height = r.u32();
    out.values = r.bytes();
    if(!r.ok) return false;

    // Guard the consumer: width*height must match what actually arrived.
    const size_t expected = static_cast<size_t>(out.width) * out.height;
    if(out.values.size() != expected)
    {
        LOG_ERROR(NET_LOG_ID, "heatmap says {}x{} but carries {} bytes",
                  out.width, out.height, out.values.size());
        return false;
    }
    return true;
}
