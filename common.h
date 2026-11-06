// Shared wire protocol and helpers for sender.cpp / receiver.cpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10+
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------- protocol --
// Video travels sender -> receiver as UDP datagrams. There is deliberately no
// retransmission: a late frame is useless. A lost frame makes the receiver
// request a new keyframe instead.

constexpr uint32_t kVideoMagic = 0x31564C53;    // "SLV1"
constexpr uint32_t kControlMagic = 0x31434C53;  // "SLC1"
constexpr int kDefaultPort = 5000;

// A standard 1500 MTU leaves 1472 bytes of UDP payload, which avoids IP
// fragmentation (a single lost IP fragment would lose the whole datagram).
constexpr int kMaxDatagram = 1472;

#pragma pack(push, 1)
struct VideoHeader {
    uint32_t magic;
    uint32_t session;    // changes whenever the sender rebuilds its pipeline
    uint32_t frameId;    // consecutive across all frames actually sent
    uint16_t fragIndex;
    uint16_t fragCount;
    uint16_t width;
    uint16_t height;
    uint8_t flags;
    uint8_t reserved[3];
    uint64_t captureUs;  // sender clock, used only for latency statistics
};

enum ControlType : uint32_t {
    kCtlHeartbeat = 1,  // sender -> receiver: lets a receiver find an idle sender
    kCtlRequestIdr = 2, // receiver -> sender
    kCtlPing = 3,       // receiver -> sender: t0 = receiver clock
    kCtlPong = 4,       // sender -> receiver: t0 echoed, t1 = sender clock
};

struct ControlPacket {
    uint32_t magic;
    uint32_t type;
    uint64_t t0;
    uint64_t t1;
};
#pragma pack(pop)

constexpr uint8_t kFlagKeyframe = 1;
constexpr int kMaxPayload = kMaxDatagram - static_cast<int>(sizeof(VideoHeader));

// ------------------------------------------------------------------- clock --
inline uint64_t nowUs() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t t = static_cast<uint64_t>(c.QuadPart);
    return (t / freq) * 1000000 + (t % freq) * 1000000 / freq;
}

// ----------------------------------------------------------------- sockets --
inline bool initWinsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

inline void tuneSocket(SOCKET s) {
    // A generous kernel buffer absorbs keyframe bursts. It cannot add latency
    // while the reader keeps up, and it costs far less than a lost frame.
    int size = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));

    // Otherwise an ICMP "port unreachable" (peer not started yet) makes the
    // next recv fail with WSAECONNRESET.
    constexpr DWORD kSioUdpConnReset = 0x9800000C;
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
}

inline void setRecvTimeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// --------------------------------------------------------------- arguments --
inline bool parseSize(const char* text, int* w, int* h) {
    return sscanf(text, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}
