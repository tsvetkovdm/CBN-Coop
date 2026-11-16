#pragma once

#include <cstdint>
#include <optional>
#include <string>

/// Transport for two-player co-op: one TCP connection, length-prefixed frames,
/// zlib for large payloads. A reader thread fills an inbox; the game thread polls it.
namespace coop::net
{

/// Start listening for a partner. Returns false and fills @p err on failure.
auto host_listen( uint16_t port, std::string &err ) -> bool;
/// Connect to a host. Blocks up to a few seconds.
auto connect_to( const std::string &host, uint16_t port, std::string &err ) -> bool;
/// Close everything (listener and connection).
auto shutdown() -> void;

/// True while a partner connection is established.
auto connected() -> bool;
/// Increases every time a new connection is established (host: partner joined).
auto connection_generation() -> int;

/// Queue a normal message for the partner. Thread-safe.
auto send( const std::string &msg ) -> void;
/// Pop one normal message. Game thread only.
auto poll( std::string &out ) -> bool;
/// Block until a normal message is available or @p ms elapsed.
auto wait_for_message( int ms ) -> bool;

/// Blobs are keyed payloads (map chunks, overmaps) that bypass the normal inbox,
/// so that any thread may wait for one.
auto send_blob( const std::string &key, const std::string &data ) -> void;
/// Take a blob from the cache if it arrived less than @p max_age_ms ago.
auto take_blob( const std::string &key, int max_age_ms ) -> std::optional<std::string>;
/// Wait until the blob with @p key arrives, then take it.
auto wait_blob( const std::string &key, int timeout_ms ) -> std::optional<std::string>;
/// Drop every cached blob.
auto clear_blobs() -> void;

} // namespace coop::net
