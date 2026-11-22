#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "character_id.h"
#include "coordinates.h"
#include "type_id.h"

class Character;
class avatar;
class mission;
class npc;
class Creature;
class JsonIn;
struct WORLDINFO;
class time_duration;
class vehicle;

/// Two-player co-op.
///
/// The host runs the world (monsters, fields, vehicles, time). The client runs a full
/// copy of the game for its own character only and reads every map chunk and overmap
/// from the host instead of from disk. Each side shows the partner as a puppet NPC.
/// Map changes travel as per-tile deltas in both directions.
namespace coop
{

enum class role_t : int { none, host, client };

auto role() -> role_t;
inline auto is_client() -> bool { return role() == role_t::client; }
inline auto is_host() -> bool { return role() == role_t::host; }
/// True when the game was started as host or client (a partner may not be connected yet).
inline auto enabled() -> bool { return role() != role_t::none; }
/// The client does not simulate the world; the host and single player do.
inline auto runs_world() -> bool { return role() != role_t::client; }

/// Command line: `--coop-host [port]` and `--coop-join <address[:port]>`.
auto configure_host( uint16_t port ) -> void;
auto configure_client( const std::string &address ) -> void;

/// Main menu dialog: host, join or switch co-op off.
auto show_menu() -> void;

/// Client: connect, do the handshake and return a local world that mirrors the host's mods.
/// Returns nullptr on failure (the reason is shown to the player).
auto client_prepare_world() -> WORLDINFO *; // *NOPAD*

/// Called once after a game was started or loaded, before the first turn.
auto on_game_started() -> void;
/// Called when the game returns to the main menu.
auto on_game_ended() -> void;

/// Top of game::do_turn(), before time advances.
auto begin_turn() -> void;
/// After the local player has finished acting in this turn.
auto before_world_sim() -> void;
/// Bottom of game::do_turn().
auto end_turn() -> void;
/// After every handled player action.
auto after_action() -> void;
/// Called from the main input wait about 20 times per second.
/// Returns true if the local player must pass the turn now.
auto idle_tick() -> bool;
/// Called while a modal window waits for input. Must not change the world.
auto background_pump() -> void;
/// True if input waits must wake up regularly to service the partner.
auto wants_pump() -> bool;

/// Client replacements for world file access.
auto client_read_map_omt( const std::string &dim, const tripoint_abs_omt &p,
                          const std::function<void( JsonIn & )> &reader ) -> bool;
auto client_read_overmap( const std::string &dim, const point_abs_om &p,
                          const std::function<void( std::istream & )> &reader ) -> bool;
/// Client: fetch the map area the window is about to move to in one request.
/// With @p full false only the chunks outside the current window are requested.
auto client_prefetch_window( const point_abs_sm &new_origin, bool full ) -> void;
/// Client: the overmap tile of the host, where a new character must start.
auto client_start_omt() -> tripoint_abs_omt;
/// The partner's puppet must not end up in the save file.
auto before_save() -> void;

/// Talking to an NPC. On the host it keeps missions meant only for the partner out of
/// the dialogue; on the client it fetches the NPC from the host, runs the dialogue
/// locally and sends the result back. Returns true if the dialogue was handled.
auto intercept_talk( npc &guy, bool radio_contact, bool enforce_first_topic ) -> bool;
/// The host does not move an NPC while the partner talks to it.
auto is_frozen( const npc &guy ) -> bool;
/// Client: create a mission on the host. Returns nullptr when not a connected client.
auto client_reserve_mission( const mission_type_id &type, const character_id &npc_id ) -> mission *; // *NOPAD*
/// Client: start a mission on the host. Returns true if handled.
auto client_assign_mission( mission &miss, avatar &u ) -> bool;
/// Host: a mission was given to the local player; offers the partner a copy if it is personal.
auto on_mission_assigned( mission &miss ) -> void;
/// A mission was completed or failed; a team mission ends for both players.
auto on_mission_finished( mission &miss ) -> void;
/// True for missions that the other player's machine runs.
auto is_other_players_mission( const mission &miss ) -> bool;

/// Light of a stand-in, which carries no real items.
auto stand_in_light( const Character &who ) -> std::optional<float>;

/// True if @p c is the local stand-in for the partner's character.
auto is_puppet( const Creature &c ) -> bool;
/// Damage dealt to the partner's puppet is forwarded to the partner. Returns true if handled.
auto intercept_damage( Character &who, Creature *source, const bodypart_id &bp, int amount ) -> bool;
/// Same for effects.
auto intercept_effect( Creature &who, const efftype_id &eff, const time_duration &dur,
                       const bodypart_str_id &bp, int intensity ) -> bool;

/// Host: the partner's character drives @p veh. Its physics then act as if the
/// local player were at the controls (cruise control, skidding).
auto partner_drives( const vehicle &veh ) -> bool;
/// Client: a driving command; the host's copy of the vehicle is the real one.
auto forward_drive( vehicle &veh, const tripoint_rel_veh &p ) -> void;

/// The main map UI was (re)created; the indicator goes on top of it.
auto on_main_ui_created() -> void;

/// Strict turn-by-turn mode for both players (key F5 by default).
auto toggle_lockstep() -> void;

/// One line for the sidebar / message log about the partner's state.
auto status_text() -> std::string;

} // namespace coop
