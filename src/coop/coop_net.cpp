#include "coop/coop_net.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <zlib.h>

#if defined(_WIN32)
#   if !defined(WIN32_LEAN_AND_MEAN)
#       define WIN32_LEAN_AND_MEAN
#   endif
#   if !defined(NOMINMAX)
#       define NOMINMAX
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
using socket_t = SOCKET;
#   define COOP_INVALID_SOCKET INVALID_SOCKET
#else
#   include <arpa/inet.h>
#   include <fcntl.h>
#   include <netdb.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/select.h>
#   include <sys/socket.h>
#   include <sys/types.h>
#   include <unistd.h>
using socket_t = int;
#   define COOP_INVALID_SOCKET (-1)
#endif

namespace coop::net
{

namespace
{

using clock_type = std::chrono::steady_clock;

constexpr auto compress_threshold = size_t{ 512 };
constexpr auto flag_zlib = uint8_t{ 1 };
constexpr auto flag_blob = uint8_t{ 2 };
constexpr auto max_frame = uint32_t{ 256u * 1024u * 1024u };

struct blob_entry {
    std::string data;
    clock_type::time_point arrived;
};

struct state {
    std::mutex sock_mutex;            // guards conn / listener handles
    socket_t listener = COOP_INVALID_SOCKET;
    socket_t conn = COOP_INVALID_SOCKET;
    std::atomic<bool> is_connected{ false };
    std::atomic<bool> stopping{ false };
    std::atomic<int> generation{ 0 };

    std::mutex send_mutex;            // serializes writers

    std::mutex inbox_mutex;
    std::condition_variable inbox_cv;
    std::deque<std::string> inbox;

    std::mutex blob_mutex;
    std::condition_variable blob_cv;
    std::map<std::string, blob_entry> blobs;

    std::thread accept_thread;
    std::thread reader_thread;

    state() = default;
    state( const state & ) = delete;
    auto operator=( const state & ) -> state & = delete; // *NOPAD*
    /// The process may exit without a clean co-op shutdown (window closed, fatal error);
    /// a joinable thread destroyed at that point would terminate the program.
    ~state() {
        stopping = true;
        is_connected = false;
        close_socket_raw( listener );
        close_socket_raw( conn );
        if( accept_thread.joinable() ) {
            accept_thread.detach();
        }
        if( reader_thread.joinable() ) {
            reader_thread.detach();
        }
    }
    static auto close_socket_raw( socket_t sock ) -> void {
        if( sock == COOP_INVALID_SOCKET ) {
            return;
        }
#if defined(_WIN32)
        ::closesocket( sock );
#else
        ::close( sock );
#endif
    }
};

auto st() -> state & // *NOPAD*
{
    static auto s = state{};
    return s;
}

auto close_socket( socket_t s ) -> void
{
    if( s == COOP_INVALID_SOCKET ) {
        return;
    }
#if defined(_WIN32)
    ::shutdown( s, SD_BOTH );
    ::closesocket( s );
#else
    ::shutdown( s, SHUT_RDWR );
    ::close( s );
#endif
}

auto platform_init() -> void
{
#if defined(_WIN32)
    static auto done = false;
    if( !done ) {
        auto data = WSADATA{};
        WSAStartup( MAKEWORD( 2, 2 ), &data );
        done = true;
    }
#endif
}

auto tune_socket( socket_t s ) -> void
{
    auto one = 1;
    ::setsockopt( s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>( &one ), sizeof( one ) );
    ::setsockopt( s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>( &one ), sizeof( one ) );
}

auto send_all( socket_t s, const char *data, size_t len ) -> bool
{
    auto sent = size_t{ 0 };
    while( sent < len ) {
#if defined(_WIN32)
        const auto n = ::send( s, data + sent, static_cast<int>( len - sent ), 0 );
#else
        const auto n = ::send( s, data + sent, len - sent, MSG_NOSIGNAL );
#endif
        if( n <= 0 ) {
            return false;
        }
        sent += static_cast<size_t>( n );
    }
    return true;
}

auto recv_all( socket_t s, char *data, size_t len ) -> bool
{
    auto got = size_t{ 0 };
    while( got < len ) {
#if defined(_WIN32)
        const auto n = ::recv( s, data + got, static_cast<int>( len - got ), 0 );
#else
        const auto n = ::recv( s, data + got, len - got, 0 );
#endif
        if( n <= 0 ) {
            return false;
        }
        got += static_cast<size_t>( n );
    }
    return true;
}

auto put_u32( std::string &out, uint32_t v ) -> void
{
    for( auto i = 0; i < 4; ++i ) {
        out.push_back( static_cast<char>( ( v >> ( 8 * i ) ) & 0xffu ) );
    }
}

auto get_u32( const char *p ) -> uint32_t
{
    auto v = uint32_t{ 0 };
    for( auto i = 0; i < 4; ++i ) {
        v |= static_cast<uint32_t>( static_cast<uint8_t>( p[i] ) ) << ( 8 * i );
    }
    return v;
}

auto send_frame( uint8_t flags, const std::string &payload ) -> void
{
    auto &s = st();
    if( !s.is_connected ) {
        return;
    }
    auto body = std::string{};
    if( payload.size() >= compress_threshold ) {
        auto bound = compressBound( static_cast<uLong>( payload.size() ) );
        auto packed = std::string( bound, '\0' );
        if( compress2( reinterpret_cast<Bytef *>( packed.data() ), &bound,
                       reinterpret_cast<const Bytef *>( payload.data() ),
                       static_cast<uLong>( payload.size() ), 1 ) == Z_OK ) {
            packed.resize( bound );
            flags |= flag_zlib;
            put_u32( body, static_cast<uint32_t>( payload.size() ) );
            body += packed;
        }
    }
    if( ( flags & flag_zlib ) == 0 ) {
        body = payload;
    }
    auto frame = std::string{};
    frame.reserve( body.size() + 5 );
    put_u32( frame, static_cast<uint32_t>( body.size() ) );
    frame.push_back( static_cast<char>( flags ) );
    frame += body;

    const auto lock = std::lock_guard<std::mutex>( s.send_mutex );
    auto conn = COOP_INVALID_SOCKET;
    {
        const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
        conn = s.conn;
    }
    if( conn == COOP_INVALID_SOCKET || !send_all( conn, frame.data(), frame.size() ) ) {
        s.is_connected = false;
    }
}

auto reader_loop( socket_t conn ) -> void
{
    auto &s = st();
    while( !s.stopping ) {
        char head[5];
        if( !recv_all( conn, head, 5 ) ) {
            break;
        }
        const auto len = get_u32( head );
        const auto flags = static_cast<uint8_t>( head[4] );
        if( len > max_frame ) {
            break;
        }
        auto body = std::string( len, '\0' );
        if( len > 0 && !recv_all( conn, body.data(), len ) ) {
            break;
        }
        auto payload = std::string{};
        if( ( flags & flag_zlib ) != 0 ) {
            if( body.size() < 4 ) {
                break;
            }
            auto raw_len = static_cast<uLongf>( get_u32( body.data() ) );
            if( raw_len > max_frame ) {
                break;
            }
            payload.resize( raw_len );
            if( uncompress( reinterpret_cast<Bytef *>( payload.data() ), &raw_len,
                            reinterpret_cast<const Bytef *>( body.data() + 4 ),
                            static_cast<uLong>( body.size() - 4 ) ) != Z_OK ) {
                break;
            }
            payload.resize( raw_len );
        } else {
            payload = std::move( body );
        }
        if( ( flags & flag_blob ) != 0 ) {
            const auto nl = payload.find( '\n' );
            if( nl == std::string::npos ) {
                continue;
            }
            {
                const auto lock = std::lock_guard<std::mutex>( s.blob_mutex );
                s.blobs[payload.substr( 0, nl )] = blob_entry{ payload.substr( nl + 1 ), clock_type::now() };
            }
            s.blob_cv.notify_all();
        } else {
            {
                const auto lock = std::lock_guard<std::mutex>( s.inbox_mutex );
                s.inbox.push_back( std::move( payload ) );
            }
            s.inbox_cv.notify_all();
        }
    }
    {
        const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
        if( s.conn == conn ) {
            s.is_connected = false;
        }
    }
    s.blob_cv.notify_all();
    s.inbox_cv.notify_all();
}

auto adopt_connection( socket_t conn ) -> void
{
    auto &s = st();
    tune_socket( conn );
    auto old = COOP_INVALID_SOCKET;
    {
        const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
        old = s.conn;
        s.conn = conn;
    }
    close_socket( old );
    if( s.reader_thread.joinable() ) {
        s.reader_thread.join();
    }
    {
        const auto lock = std::lock_guard<std::mutex>( s.inbox_mutex );
        s.inbox.clear();
    }
    clear_blobs();
    s.is_connected = true;
    ++s.generation;
    s.reader_thread = std::thread( reader_loop, conn );
}

auto accept_loop() -> void
{
    auto &s = st();
    while( !s.stopping ) {
        auto listener = COOP_INVALID_SOCKET;
        {
            const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
            listener = s.listener;
        }
        if( listener == COOP_INVALID_SOCKET ) {
            break;
        }
        const auto conn = ::accept( listener, nullptr, nullptr );
        if( conn == COOP_INVALID_SOCKET ) {
            if( s.stopping ) {
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            continue;
        }
        adopt_connection( conn );
    }
}

} // namespace

auto host_listen( const uint16_t port, std::string &err ) -> bool
{
    platform_init();
    auto &s = st();
    s.stopping = false;
    const auto listener = ::socket( AF_INET, SOCK_STREAM, 0 );
    if( listener == COOP_INVALID_SOCKET ) {
        err = "cannot create socket";
        return false;
    }
    auto one = 1;
    ::setsockopt( listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>( &one ),
                  sizeof( one ) );
    auto addr = sockaddr_in{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( port );
    if( ::bind( listener, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) != 0 ||
        ::listen( listener, 1 ) != 0 ) {
        close_socket( listener );
        err = "cannot listen on port " + std::to_string( port );
        return false;
    }
    {
        const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
        s.listener = listener;
    }
    s.accept_thread = std::thread( accept_loop );
    return true;
}

auto connect_to( const std::string &host, const uint16_t port, std::string &err ) -> bool
{
    platform_init();
    auto &s = st();
    s.stopping = false;
    auto hints = addrinfo{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if( ::getaddrinfo( host.c_str(), std::to_string( port ).c_str(), &hints, &res ) != 0 ||
        res == nullptr ) {
        err = "cannot resolve " + host;
        return false;
    }
    const auto conn = ::socket( res->ai_family, res->ai_socktype, res->ai_protocol );
    if( conn == COOP_INVALID_SOCKET ) {
        ::freeaddrinfo( res );
        err = "cannot create socket";
        return false;
    }
    const auto rc = ::connect( conn, res->ai_addr, static_cast<int>( res->ai_addrlen ) );
    ::freeaddrinfo( res );
    if( rc != 0 ) {
        close_socket( conn );
        err = "cannot connect to " + host + ":" + std::to_string( port );
        return false;
    }
    adopt_connection( conn );
    return true;
}

auto shutdown() -> void
{
    auto &s = st();
    s.stopping = true;
    auto listener = COOP_INVALID_SOCKET;
    auto conn = COOP_INVALID_SOCKET;
    {
        const auto sl = std::lock_guard<std::mutex>( s.sock_mutex );
        listener = s.listener;
        conn = s.conn;
        s.listener = COOP_INVALID_SOCKET;
        s.conn = COOP_INVALID_SOCKET;
    }
    s.is_connected = false;
    close_socket( listener );
    close_socket( conn );
    if( s.accept_thread.joinable() ) {
        s.accept_thread.join();
    }
    if( s.reader_thread.joinable() ) {
        s.reader_thread.join();
    }
}

auto connected() -> bool
{
    return st().is_connected;
}

auto connection_generation() -> int
{
    return st().generation;
}

auto send( const std::string &msg ) -> void
{
    send_frame( 0, msg );
}

auto poll( std::string &out ) -> bool
{
    auto &s = st();
    const auto lock = std::lock_guard<std::mutex>( s.inbox_mutex );
    if( s.inbox.empty() ) {
        return false;
    }
    out = std::move( s.inbox.front() );
    s.inbox.pop_front();
    return true;
}

auto wait_for_message( const int ms ) -> bool
{
    auto &s = st();
    auto lock = std::unique_lock<std::mutex>( s.inbox_mutex );
    return s.inbox_cv.wait_for( lock, std::chrono::milliseconds( ms ), [&s] {
        return !s.inbox.empty() || !s.is_connected;
    } ) && !s.inbox.empty();
}

auto send_blob( const std::string &key, const std::string &data ) -> void
{
    send_frame( flag_blob, key + "\n" + data );
}

auto take_blob( const std::string &key, const int max_age_ms ) -> std::optional<std::string>
{
    auto &s = st();
    const auto lock = std::lock_guard<std::mutex>( s.blob_mutex );
    const auto it = s.blobs.find( key );
    if( it == s.blobs.end() ) {
        return std::nullopt;
    }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         clock_type::now() - it->second.arrived ).count();
    auto data = std::move( it->second.data );
    s.blobs.erase( it );
    if( max_age_ms >= 0 && age > max_age_ms ) {
        return std::nullopt;
    }
    return data;
}

auto wait_blob( const std::string &key, const int timeout_ms ) -> std::optional<std::string>
{
    auto &s = st();
    auto lock = std::unique_lock<std::mutex>( s.blob_mutex );
    const auto ok = s.blob_cv.wait_for( lock, std::chrono::milliseconds( timeout_ms ), [&] {
        return s.blobs.contains( key ) || !s.is_connected;
    } );
    if( !ok ) {
        return std::nullopt;
    }
    const auto it = s.blobs.find( key );
    if( it == s.blobs.end() ) {
        return std::nullopt;
    }
    auto data = std::move( it->second.data );
    s.blobs.erase( it );
    return data;
}

auto clear_blobs() -> void
{
    auto &s = st();
    const auto lock = std::lock_guard<std::mutex>( s.blob_mutex );
    s.blobs.clear();
}

} // namespace coop::net
