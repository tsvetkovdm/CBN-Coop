#include "coop/coop.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "action.h"
#include "artifact.h"
#include "avatar.h"
#include "calendar.h"
#include "character_functions.h"
#include "coop/coop_net.h"
#include "coordinates.h"
#include "color.h"
#include "cursesdef.h"
#include "creature_tracker.h"
#include "debug.h"
#include "fstream_utils.h"
#include "game.h"
#include "input.h"
#include "item.h"
#include "json.h"
#include "map/field.h"
#include "map/map.h"
#include "map/mapbuffer.h"
#include "map/mapbuffer_registry.h"
#include "map/submap.h"
#include "messages.h"
#include "mod_manager.h"
#include "rng.h"
#include "kill_tracker.h"
#include "mission.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap/overmap.h"
#include "overmap/overmapbuffer.h"
#include "panels.h"
#include "path_info.h"
#include "player_activity.h"
#include "popup.h"
#include "string_input_popup.h"
#include "ui.h"
#include "sounds.h"
#include "trap.h"
#include "units.h"
#include "vehicle/veh_type.h"
#include "vehicle/vehicle.h"
#include "vehicle/vehicle_part.h"
#include "vehicle/vpart_position.h"
#include "vehicle/vpart_range.h"
#include "ui_manager.h"
#include "world.h"
#include "worldfactory.h"

/// Co-op needs to assign missions to a character that is not the local avatar.
struct coop_mission_access {
    static auto player( const mission &m ) -> character_id {
        return m.player_id;
    }
    static auto set_player( mission &m, const character_id &who ) -> void {
        m.player_id = who;
    }
    /// Host: what mission::assign does, for the partner. Starting a mission changes the world.
    static auto start_for( mission &m, const character_id &who ) -> void {
        m.player_id = who;
        if( m.status != mission::mission_status::yet_to_start ) {
            return;
        }
        if( m.type->deadline_low != 0_turns || m.type->deadline_high != 0_turns ) {
            m.deadline = calendar::turn + rng( m.type->deadline_low, m.type->deadline_high );
        } else {
            m.deadline = calendar::start_of_cataclysm;
        }
        m.type->start( &m );
        m.status = mission::mission_status::in_progress;
    }
    /// Client: the host started the mission, now it belongs to the local avatar.
    static auto finish_on( mission &m, avatar &u ) -> void {
        m.player_id = u.getID();
        u.on_mission_assignment( m );
        const auto &kills = g->get_kill_tracker();
        if( m.type->goal == MGOAL_KILL_MONSTER_TYPE && m.monster_type != mtype_id::NULL_ID() ) {
            m.kill_count_to_reach = kills.kill_count( m.monster_type ) + m.monster_kill_goal;
        } else if( m.type->goal == MGOAL_KILL_MONSTER_SPEC ) {
            m.kill_count_to_reach = kills.kill_count( m.monster_species ) + m.monster_kill_goal;
        }
    }
};

namespace coop
{

namespace
{

using clock_type = std::chrono::steady_clock;

/// How the other player's character is known on each machine. Avatars on both
/// machines usually have the same small id, so the stand-ins use fixed large ids.
const auto host_stand_in_id = character_id( 2000000001 );
const auto partner_id = character_id( 2000000002 );

constexpr auto default_port = uint16_t{ 4580 };
constexpr auto protocol_version = 1;
/// How many turns the client may run ahead of the host while nobody is in danger.
constexpr auto max_client_lead = 120;
/// A player who pressed a key less than this long ago is "active" and is not auto-passed.
constexpr auto active_window_ms = 1500;
/// An idle player this many turns behind waits the gap out in one go.
constexpr auto catch_up_min_turns = 10;
/// Marks the wait that catches up with the partner (no "You finish waiting.").
constexpr auto catch_up_tag = "coop_catch_up";
/// Tile scan radius around each player, in submaps.
constexpr auto scan_radius = 5;

using tile_sigs = std::array<uint64_t, SEEX *SEEY>;

struct peer_state {
    bool in_game = false;
    int turn = 0;          // partner's calendar turn
    int done = -1;         // last turn in which the partner finished acting
    bool danger = false;
    bool active = false;   // partner pressed keys recently
    std::string name;
    bool male = true;
    tripoint_abs_ms pos;
    bool has_pos = false;
    std::string activity;
    std::vector<std::string> worn;
    std::string wield;
    std::vector<std::string> traits;
    float light = 0.0f;
    bool in_veh = false;   // sits in a vehicle
    bool ctrl = false;     // drives it
    std::string veh;       // co-op id of that vehicle
    tripoint_mnt_veh mount; // the seat in it
};

/// A sound made on this machine, waiting to be sent.
struct sound_record {
    tripoint_abs_ms pos;
    sound_event se;
};

/// What was last synced about one vehicle.
struct veh_state {
    uint64_t motion = 0;
    uint64_t content = 0;
    uint64_t fuel = 0;
    int full_turn = -1000000;
    tripoint_abs_ms pos;
};

struct session {
    role_t role = role_t::none;
    uint16_t port = default_port;
    std::string address;
    bool in_game = false;
    int net_generation = 0;

    // welcome data (client)
    std::string host_world;
    int host_turn_at_join = 0;
    tripoint_abs_ms host_pos;
    tripoint_abs_ms spawn_pos;         // where the host wants the client to appear

    peer_state peer;
    shared_ptr_fast<npc> puppet;
    tripoint_abs_ms puppet_set_pos;

    bool my_danger = false;
    clock_type::time_point last_input = clock_type::now() - std::chrono::hours( 1 );
    int last_done_sent = -1;
    bool autopass = false;
    int bulk_counter = 0;
    bool screen_dirty = false;

    std::map<tripoint_abs_sm, tile_sigs> sigs;

    // monster replication
    int next_mon_id = 1;
    std::map<const monster *, std::pair<weak_ptr_fast<monster>, int>> mon_ids; // host
    std::map<int, weak_ptr_fast<monster>> mon_by_id;                           // both
    std::map<int, uint64_t> mon_sig;                                           // last synced sig
    bool applying_remote = false;

    // NPC stand-ins on the client, keyed by the host's NPC id
    std::map<int, shared_ptr_fast<npc>> ghosts;
    std::map<int, uint64_t> npc_sig;   // host: last sent state per NPC
    uint64_t puppet_look = 0;          // look currently applied to the puppet
    bool building = false;             // true while a stand-in is being dressed
    bool transferring = false;         // true while the client is moved next to the host

    /// Strict turn-by-turn mode, shared by both players.
    bool lockstep = false;

    // indicator at the top of the map
    std::unique_ptr<ui_adaptor> hud;
    catacurses::window hud_win;
    bool waiting_now = false;
    std::string banner;
    clock_type::time_point banner_until;

    // dialogue with NPCs
    character_id talking_npc;          // host: NPC frozen while the partner talks to it
    character_id host_talking;         // host: NPC the host talks to
    character_id partner_avatar;       // host: id of the partner's avatar on the partner's machine
    bool in_talk_body = false;         // the real dialogue is running
    int req_counter = 0;
    /// Personal mission copies made during a dialogue; they appear after it.
    std::vector<std::pair<int, std::string>> pending_copies;

    /// Light of stand-ins: their real items stay with their owner.
    std::map<const Character *, float> lights;
    bool danger_hint_shown = false;
    bool resync_requested = false;

    // sounds made here since the last send; the partner's sounds are played without a record
    std::vector<sound_record> sounds;
    bool playing_partner_sound = false;

    // vehicles, keyed by the co-op id stored in a vehicle tag
    std::map<std::string, veh_state> vehs;
    std::map<std::string, int> veh_requested;   // client: when a vehicle was asked for
};

auto s() -> session & // *NOPAD*
{
    static auto inst = session{};
    return inst;
}

auto my_turn() -> int
{
    return to_turn<int>( calendar::turn );
}

auto ms_since( const clock_type::time_point &t ) -> int
{
    return static_cast<int>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                 clock_type::now() - t ).count() );
}

// --- hashing ---------------------------------------------------------------

auto fnv( uint64_t h, const uint64_t v ) -> uint64_t
{
    for( auto i = 0; i < 8; ++i ) {
        h ^= ( v >> ( 8 * i ) ) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}

auto fnv_str( uint64_t h, const std::string &str ) -> uint64_t
{
    for( const auto c : str ) {
        h ^= static_cast<uint8_t>( c );
        h *= 1099511628211ull;
    }
    return h;
}

constexpr auto fnv_seed = uint64_t{ 1469598103934665603ull };

auto item_sig( uint64_t h, const item &it ) -> uint64_t
{
    h = fnv_str( h, it.typeId().str() );
    h = fnv( h, static_cast<uint64_t>( it.charges ) );
    h = fnv( h, static_cast<uint64_t>( it.damage() ) );
    for( const item *sub : it.contents.all_items_top() ) {
        h = item_sig( h, *sub );
    }
    return h;
}

auto tile_sig( const submap &sm, const point_sm_ms &p ) -> uint64_t
{
    auto h = fnv_seed;
    h = fnv( h, static_cast<uint64_t>( sm.get_ter( p ).to_i() ) );
    h = fnv( h, static_cast<uint64_t>( sm.get_furn( p ).to_i() ) );
    h = fnv( h, static_cast<uint64_t>( sm.get_trap( p ).to_i() ) );
    for( const auto &fld : sm.get_field( p ) ) {
        if( !fld.second.is_field_alive() ) {
            continue;
        }
        h = fnv( h, static_cast<uint64_t>( fld.first.to_i() ) );
        h = fnv( h, static_cast<uint64_t>( fld.second.get_field_intensity() ) );
    }
    for( const item *it : sm.get_items( p ) ) {
        h = item_sig( h, *it );
    }
    return h;
}

// --- JSON helpers ----------------------------------------------------------

struct msg_writer {
    std::ostringstream os;
    JsonOut jo;
    explicit msg_writer( const std::string &type ) : jo( os ) {
        jo.start_object();
        jo.member( "t", type );
    }
    auto send() -> void {
        jo.end_object();
        net::send( os.str() );
    }
};

auto write_pos( JsonOut &jo, const std::string &name, const tripoint_abs_ms &p ) -> void
{
    jo.member( name );
    jo.start_array();
    jo.write( p.x() );
    jo.write( p.y() );
    jo.write( p.z() );
    jo.end_array();
}

auto read_pos( const JsonObject &o, const std::string &name ) -> tripoint_abs_ms
{
    auto a = o.get_array( name );
    const auto x = a.next_int();
    const auto y = a.next_int();
    const auto z = a.next_int();
    return tripoint_abs_ms( x, y, z );
}

// --- map chunk service (host) ---------------------------------------------

auto omt_key( const std::string &dim, const tripoint_abs_omt &p ) -> std::string
{
    return string_format( "omt|%s|%d|%d|%d", dim, p.x(), p.y(), p.z() );
}

auto om_key( const std::string &dim, const point_abs_om &p ) -> std::string
{
    return string_format( "om|%s|%d|%d", dim, p.x(), p.y() );
}

auto host_tag_vehicle( vehicle &v ) -> std::string;

auto host_serialize_omt( const std::string &dim, const tripoint_abs_omt &omt ) -> std::string
{
    auto &buffer = MAPBUFFER_REGISTRY.get( dimension_id( dim ) );
    auto os = std::ostringstream{};
    auto jo = JsonOut( os );
    jo.start_array();
    for( const point &off : { point_zero, point_south, point_east, point_south_east } ) {
        const auto addr = project_to<coords::sm>( omt ) + off;
        auto *sm = buffer.get_submap( addr, { .mode = mapbuffer_lookup_mode::load_or_generate } );
        if( sm == nullptr ) {
            continue;
        }
        jo.start_object();
        jo.member( "version", savegame_version );
        jo.member( "coordinates" );
        jo.start_array();
        jo.write( addr.x() );
        jo.write( addr.y() );
        jo.write( addr.z() );
        jo.end_array();
        for( const auto &veh : sm->vehicles ) {
            if( veh ) {
                host_tag_vehicle( *veh );
            }
        }
        sm->store( jo );
        jo.end_object();
    }
    jo.end_array();
    return os.str();
}

auto host_serialize_overmap( const std::string &dim, const point_abs_om &p ) -> std::string
{
    auto &om = get_overmapbuffer( dimension_id( dim ) ).get( p );
    om.save();
    auto data = std::string{};
    g->get_active_world()->read_overmap( dim, p, [&data]( std::istream & in ) {
        data.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    } );
    return data;
}

auto host_handle_get( const JsonObject &o ) -> void
{
    const auto kind = o.get_string( "k" );
    const auto dim = o.get_string( "dim", "" );
    if( kind == "omt" ) {
        const auto p = tripoint_abs_omt( o.get_int( "x" ), o.get_int( "y" ), o.get_int( "z" ) );
        net::send_blob( omt_key( dim, p ), host_serialize_omt( dim, p ) );
    } else if( kind == "om" ) {
        const auto p = point_abs_om( o.get_int( "x" ), o.get_int( "y" ) );
        net::send_blob( om_key( dim, p ), host_serialize_overmap( dim, p ) );
    } else if( kind == "artifacts" ) {
        // randomly generated item types live in a world file
        auto data = std::string{};
        ::save_artifacts( g->get_active_world(), "artifacts.gsav" );
        g->get_active_world()->read_from_file( "artifacts.gsav", [&data]( std::istream & in ) {
            data.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        }, true );
        net::send_blob( "artifacts", data );
    } else if( kind == "bulk" ) {
        // every map chunk inside [x0,x1) x [y0,y1) that is not inside the "old" rectangle
        const auto x0 = o.get_int( "x0" );
        const auto y0 = o.get_int( "y0" );
        const auto x1 = o.get_int( "x1" );
        const auto y1 = o.get_int( "y1" );
        const auto ox0 = o.get_int( "ox0", 0 );
        const auto oy0 = o.get_int( "oy0", 0 );
        const auto ox1 = o.get_int( "ox1", 0 );
        const auto oy1 = o.get_int( "oy1", 0 );
        for( auto x = x0; x < x1; ++x ) {
            for( auto y = y0; y < y1; ++y ) {
                if( x >= ox0 && x < ox1 && y >= oy0 && y < oy1 ) {
                    continue;
                }
                for( auto z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
                    const auto p = tripoint_abs_omt( x, y, z );
                    net::send_blob( omt_key( dim, p ), host_serialize_omt( dim, p ) );
                }
            }
        }
        net::send_blob( o.get_string( "id" ), "done" );
    }
}

// --- puppet ----------------------------------------------------------------

/// What a character looks like to the partner: clothes and the wielded item.
struct look_t {
    std::vector<std::string> worn;
    std::string wield;
    /// Mutations, which include hair, eyes and skin.
    std::vector<std::string> traits;
    /// Light from carried items (flashlights, phones, lanterns).
    float light = 0.0f;
};

auto look_of( const Character &who ) -> look_t
{
    auto res = look_t{};
    for( const item *it : who.worn ) {
        res.worn.push_back( it->typeId().str() );
    }
    const auto &weapon = who.primary_weapon();
    if( !weapon.is_null() ) {
        res.wield = weapon.typeId().str();
    }
    for( const auto &trait : who.get_mutations( true ) ) {
        res.traits.push_back( trait.str() );
    }
    res.light = who.active_light();
    return res;
}

auto look_sig( const look_t &l ) -> uint64_t
{
    auto h = fnv_seed;
    for( const auto &id : l.worn ) {
        h = fnv_str( h, id );
        h = fnv( h, 1 );
    }
    h = fnv_str( h, l.wield );
    for( const auto &id : l.traits ) {
        h = fnv_str( h, id );
        h = fnv( h, 2 );
    }
    return fnv( h, static_cast<uint64_t>( l.light * 10.0f ) );
}

auto write_look( JsonOut &jo, const look_t &l ) -> void
{
    jo.member( "worn", l.worn );
    jo.member( "wield", l.wield );
    jo.member( "traits", l.traits );
    jo.member( "light", l.light );
}

auto read_look( const JsonObject &o ) -> look_t
{
    auto res = look_t{};
    if( o.has_array( "worn" ) ) {
        auto a = o.get_array( "worn" );
        while( a.has_more() ) {
            res.worn.push_back( a.next_string() );
        }
    }
    res.wield = o.get_string( "wield", "" );
    if( o.has_array( "traits" ) ) {
        auto a = o.get_array( "traits" );
        while( a.has_more() ) {
            res.traits.push_back( a.next_string() );
        }
    }
    res.light = static_cast<float>( o.get_float( "light", 0.0 ) );
    return res;
}

/// Dress a stand-in. The items are fresh copies; only their types matter.
auto apply_look( npc &guy, const look_t &l ) -> void
{
    auto &st = s();
    st.building = true;
    // mutations first: some of them do not allow certain clothes
    auto wanted = std::set<trait_id> {};
    for( const auto &id : l.traits ) {
        if( const auto trait = trait_id( id ); trait.is_valid() ) {
            wanted.insert( trait );
        }
    }
    for( const auto &trait : guy.get_mutations( true ) ) {
        if( !wanted.contains( trait ) ) {
            guy.unset_mutation( trait );
        }
    }
    for( const auto &trait : wanted ) {
        if( !guy.has_trait( trait ) ) {
            guy.set_mutation( trait );
        }
    }
    st.lights[&guy] = l.light;
    guy.worn.clear();
    for( const auto &id : l.worn ) {
        const auto type = itype_id( id );
        if( type.is_valid() ) {
            guy.worn.push_back( item::spawn( type ) );
        }
    }
    guy.remove_primary_weapon();
    if( const auto type = itype_id( l.wield ); !l.wield.empty() && type.is_valid() ) {
        guy.set_primary_weapon( item::spawn( type ) );
    }
    st.building = false;
}

enum class stance_t : int { ally, neutral, hostile };

/// Create a local stand-in NPC for a character that another machine controls.
auto make_stand_in( const std::string &name, const bool male, const tripoint_abs_ms &pos,
                    const character_id &id ) -> shared_ptr_fast<npc>
{
    auto &st = s();
    st.building = true;
    auto guy = make_shared_fast<npc>();
    character_funcs::normalize( *guy );
    // the game tells NPCs apart by id; NPC stand-ins use the host's ids, so that
    // missions and dialogue refer to the same NPC on both machines
    guy->setID( id );
    guy->name = name;
    guy->male = male;
    guy->set_mission( NPC_MISSION_NULL );
    const auto proj = project_remain<coords::sm>( pos );
    guy->spawn_at_precise( proj.quotient, proj.remainder_tripoint );
    get_overmapbuffer( g->get_current_dimension_id() ).insert_npc( guy );
    st.building = false;
    return guy;
}

auto set_stance( npc &guy, const stance_t stance ) -> void
{
    if( stance == stance_t::ally ) {
        guy.set_fac( faction_id( "your_followers" ) );
        guy.set_attitude( NPCATT_FOLLOW );
    } else if( stance == stance_t::hostile ) {
        guy.set_fac( faction_id( "no_faction" ) );
        guy.set_attitude( NPCATT_KILL );
    } else {
        guy.set_fac( faction_id( "no_faction" ) );
        guy.set_attitude( NPCATT_NULL );
    }
}

auto remove_stand_in( shared_ptr_fast<npc> &guy ) -> void
{
    if( !guy ) {
        return;
    }
    s().lights.erase( guy.get() );
    const auto id = guy->getID();
    get_overmapbuffer( g->get_current_dimension_id() ).remove_npc( id );
    const auto &npcs = g->raw_npcs();
    if( std::ranges::any_of( npcs, [id]( const auto & n ) { return n->getID() == id; } ) ) {
        g->erase_npc( id );
    }
    guy.reset();
}

auto remove_puppet() -> void
{
    remove_stand_in( s().puppet );
    s().puppet_look = 0;
}

auto remove_ghosts() -> void
{
    auto &st = s();
    for( auto &entry : st.ghosts ) {
        remove_stand_in( entry.second );
    }
    st.ghosts.clear();
}

/// The game unloads NPCs that leave the map window; a stand-in must then be made again.
auto is_active( const npc *guy ) -> bool
{
    if( guy == nullptr ) {
        return false;
    }
    for( npc &other : g->all_npcs() ) {
        if( &other == guy ) {
            return true;
        }
    }
    return false;
}

// --- boarding ----------------------------------------------------------------

/// Board a seat without the side effects of map::board_vehicle (it complains
/// about taken seats and moves the character).
auto quiet_board( Character &who ) -> bool
{
    auto vp = g->m.veh_at( who.bub_pos() ).part_with_feature( VPFLAG_BOARDABLE, true );
    if( !vp ) {
        return false;
    }
    auto &part = vp->part();
    if( part.has_flag( vehicle_part::passenger_flag ) && part.passenger_id != who.getID() ) {
        return false;
    }
    part.set_flag( vehicle_part::passenger_flag );
    part.passenger_id = who.getID();
    vp->vehicle().invalidate_mass();
    who.in_vehicle = true;
    return true;
}

/// Leave a seat without the side effects of map::unboard_vehicle (it makes a
/// vehicle skid when its driver leaves).
auto quiet_unboard( Character &who ) -> void
{
    if( auto vp = g->m.veh_at( who.bub_pos() ).part_with_feature( VPFLAG_BOARDABLE, false ) ) {
        auto &part = vp->part();
        if( part.has_flag( vehicle_part::passenger_flag ) && part.passenger_id == who.getID() ) {
            part.remove_flag( vehicle_part::passenger_flag );
            vp->vehicle().invalidate_mass();
        }
    }
    who.in_vehicle = false;
}

/// Keep a stand-in seated when the other player sits in a vehicle, so moving
/// vehicles carry it instead of running it over.
auto sync_boarding( npc &guy, const bool wants_in ) -> void
{
    if( wants_in && !guy.in_vehicle ) {
        quiet_board( guy );
    } else if( !wants_in && guy.in_vehicle ) {
        quiet_unboard( guy );
    }
}

auto find_vehicle( const std::string &id ) -> vehicle *;
auto veh_id_of( const vehicle &v ) -> std::string;

/// Where the seat with the given mount point is now.
auto seat_location( vehicle &v, const tripoint_mnt_veh &mount ) -> std::optional<tripoint_abs_ms>
{
    for( auto i = 0; i < v.part_count(); ++i ) {
        const auto &p = v.part( i );
        if( !p.removed && p.mount == mount && p.info().has_flag( VPFLAG_BOARDABLE ) ) {
            return bub_to_abs( v.bub_part_location( i ) );
        }
    }
    return std::nullopt;
}

auto ensure_puppet() -> void
{
    auto &st = s();
    if( st.puppet || !st.peer.has_pos || !g->m.inbounds( st.peer.pos ) ) {
        return;
    }
    DebugLog( DL::Info, DC::Main ) << "coop: partner shown at " << st.peer.pos.to_string();
    st.puppet = make_stand_in( st.peer.name.empty() ? std::string( "Partner" ) : st.peer.name,
                               st.peer.male, st.peer.pos, is_host() ? partner_id : host_stand_in_id );
    set_stance( *st.puppet, stance_t::ally );
    st.puppet_set_pos = st.peer.pos;
    g->load_npcs();
}

auto update_puppet() -> void
{
    auto &st = s();
    if( !st.peer.in_game || !st.peer.has_pos ) {
        return;
    }
    if( !g->m.inbounds( st.peer.pos ) ) {
        // the partner is outside the local map window
        if( st.puppet ) {
            DebugLog( DL::Info, DC::Main ) << "coop: partner left the map window";
        }
        remove_puppet();
        return;
    }
    if( st.puppet && !is_active( st.puppet.get() ) ) {
        remove_puppet();
    }
    ensure_puppet();
    if( !st.puppet ) {
        return;
    }
    // In a vehicle the partner's seat is found on the local copy of the vehicle:
    // the two machines see a moving vehicle a moment apart.
    auto seat = std::optional<tripoint_abs_ms> {};
    if( st.peer.in_veh && !st.peer.veh.empty() ) {
        if( auto *v = find_vehicle( st.peer.veh ) ) {
            seat = seat_location( *v, st.peer.mount );
        }
    }
    const auto target = seat.value_or( st.peer.pos );
    if( st.puppet->abs_pos() != target ) {
        // keep the puppet where it is until the tile is free of other NPCs
        const auto *blocker = g->critter_at<npc>( abs_to_bub( target ) );
        if( blocker == nullptr || blocker == st.puppet.get() ) {
            if( st.puppet->in_vehicle ) {
                quiet_unboard( *st.puppet );
            }
            st.puppet->setpos( target );
        }
    }
    st.puppet_set_pos = st.peer.pos;
    sync_boarding( *st.puppet, seat.has_value() && st.puppet->abs_pos() == target );
    st.puppet->controlling_vehicle = st.puppet->in_vehicle && st.peer.ctrl;
    const auto wanted = look_t{ .worn = st.peer.worn, .wield = st.peer.wield, .traits = st.peer.traits, .light = st.peer.light };
    if( const auto sig = look_sig( wanted ); sig != st.puppet_look ) {
        apply_look( *st.puppet, wanted );
        st.puppet_look = sig;
    }
    st.puppet->moves = 0;
}

// --- NPCs of the host --------------------------------------------------------

auto host_send_npcs( const bool full ) -> void
{
    auto &st = s();
    if( !net::connected() || !st.peer.in_game ) {
        return;
    }
    auto w = msg_writer( "npcs" );
    auto alive = std::set<int> {};
    auto count = 0;
    w.jo.member( "up" );
    w.jo.start_array();
    for( npc &guy : g->all_npcs() ) {
        if( &guy == st.puppet.get() || guy.is_dead() ) {
            continue;
        }
        const auto id = guy.getID().get_value();
        alive.insert( id );
        const auto look = look_of( guy );
        const auto stance = guy.is_enemy() ? stance_t::hostile :
                            guy.is_player_ally() ? stance_t::ally : stance_t::neutral;
        const auto pos = guy.abs_pos();
        auto sig = look_sig( look );
        sig = fnv( sig, static_cast<uint64_t>( pos.x() ) );
        sig = fnv( sig, static_cast<uint64_t>( pos.y() ) );
        sig = fnv( sig, static_cast<uint64_t>( pos.z() ) );
        sig = fnv( sig, static_cast<uint64_t>( stance ) );
        const auto old = st.npc_sig.find( id );
        if( !full && old != st.npc_sig.end() && old->second == sig ) {
            continue;
        }
        st.npc_sig[id] = sig;
        w.jo.start_object();
        w.jo.member( "id", id );
        w.jo.member( "name", guy.name );
        w.jo.member( "male", guy.male );
        w.jo.member( "stance", static_cast<int>( stance ) );
        write_pos( w.jo, "pos", pos );
        write_look( w.jo, look );
        w.jo.end_object();
        ++count;
    }
    w.jo.end_array();
    w.jo.member( "rm" );
    w.jo.start_array();
    for( auto it = st.npc_sig.begin(); it != st.npc_sig.end(); ) {
        if( !alive.contains( it->first ) ) {
            w.jo.write( it->first );
            it = st.npc_sig.erase( it );
            ++count;
        } else {
            ++it;
        }
    }
    w.jo.end_array();
    if( count > 0 ) {
        w.send();
    }
}

auto client_apply_npcs( const JsonObject &o ) -> void
{
    auto &st = s();
    auto created = false;
    auto up = o.get_array( "up" );
    while( up.has_more() ) {
        auto one = up.next_object();
        one.allow_omitted_members();
        const auto id = one.get_int( "id" );
        const auto pos = read_pos( one, "pos" );
        const auto stance = static_cast<stance_t>( one.get_int( "stance", 1 ) );
        auto &guy = st.ghosts[id];
        if( !g->m.inbounds( pos ) ) {
            remove_stand_in( guy );
            st.ghosts.erase( id );
            continue;
        }
        if( guy && !is_active( guy.get() ) ) {
            remove_stand_in( guy );
        }
        if( !guy ) {
            guy = make_stand_in( one.get_string( "name" ), one.get_bool( "male" ), pos, character_id( id ) );
            created = true;
        } else if( guy->abs_pos() != pos && pos != g->u.abs_pos() ) {
            // never on the local player's tile; the next update moves it on
            guy->setpos( pos );
        }
        set_stance( *guy, stance );
        apply_look( *guy, read_look( one ) );
        guy->moves = 0;
    }
    auto rm = o.get_array( "rm" );
    while( rm.has_more() ) {
        const auto id = rm.next_int();
        if( const auto it = st.ghosts.find( id ); it != st.ghosts.end() ) {
            remove_stand_in( it->second );
            st.ghosts.erase( it );
        }
    }
    if( created ) {
        g->load_npcs();
    }
}

auto ghost_id_of( const Creature &c ) -> int
{
    for( const auto &entry : s().ghosts ) {
        if( static_cast<const Creature *>( entry.second.get() ) == &c ) {
            return entry.first;
        }
    }
    return -1;
}

// --- state message ---------------------------------------------------------

auto compute_danger() -> bool
{
    return g->is_hostile_nearby() != nullptr;
}

auto send_state( const int done ) -> void
{
    auto &st = s();
    if( !net::connected() || !st.in_game ) {
        return;
    }
    st.my_danger = compute_danger();
    st.last_done_sent = std::max( st.last_done_sent, done );
    auto w = msg_writer( "st" );
    w.jo.member( "turn", my_turn() );
    w.jo.member( "done", st.last_done_sent );
    w.jo.member( "danger", st.my_danger );
    w.jo.member( "active", ms_since( st.last_input ) < active_window_ms );
    w.jo.member( "name", g->u.name );
    w.jo.member( "male", g->u.male );
    w.jo.member( "me", g->u.getID().get_value() );
    write_pos( w.jo, "pos", g->u.abs_pos() );
    w.jo.member( "act", g->u.activity && *g->u.activity ? g->u.activity->id().str() : std::string() );
    w.jo.member( "inveh", g->u.in_vehicle );
    w.jo.member( "ctrl", g->u.controlling_vehicle );
    if( g->u.in_vehicle ) {
        if( const auto vp = g->m.veh_at( g->u.bub_pos() ) ) {
            w.jo.member( "veh", veh_id_of( vp->vehicle() ) );
            w.jo.member( "mnt" );
            w.jo.start_array();
            w.jo.write( vp->mount().x() );
            w.jo.write( vp->mount().y() );
            w.jo.write( vp->mount().z() );
            w.jo.end_array();
        }
    }
    write_look( w.jo, look_of( g->u ) );
    w.send();
}

// --- tile deltas -----------------------------------------------------------

auto write_tile( JsonOut &jo, const tripoint_abs_ms &abs, const submap &sm,
                 const point_sm_ms &l ) -> void
{
    jo.start_object();
    write_pos( jo, "p", abs );
    jo.member( "ter", sm.get_ter( l ).id().str() );
    jo.member( "furn", sm.get_furn( l ).id().str() );
    jo.member( "trap", sm.get_trap( l ).id().str() );
    jo.member( "f" );
    jo.start_array();
    for( const auto &fld : sm.get_field( l ) ) {
        if( !fld.second.is_field_alive() ) {
            continue;
        }
        jo.start_array();
        jo.write( fld.first.id().str() );
        jo.write( fld.second.get_field_intensity() );
        jo.write( to_turns<int>( fld.second.get_field_age() ) );
        jo.end_array();
    }
    jo.end_array();
    auto ios = std::ostringstream{};
    auto ijo = JsonOut( ios );
    ijo.start_array();
    for( const item *it : sm.get_items( l ) ) {
        it->serialize( ijo );
    }
    ijo.end_array();
    jo.member( "i", ios.str() );
    jo.end_object();
}

auto apply_tile( const JsonObject &o ) -> void
{
    const auto abs = read_pos( o, "p" );
    if( !g->m.inbounds( abs ) ) {
        return;
    }
    auto &here = g->m;
    const auto p = abs_to_bub( abs );
    const auto ter = ter_str_id( o.get_string( "ter" ) );
    if( ter.is_valid() && here.ter( p ) != ter.id() ) {
        here.ter_set( p, ter.id() );
    }
    const auto furn = furn_str_id( o.get_string( "furn" ) );
    if( furn.is_valid() && here.furn( p ) != furn.id() ) {
        here.furn_set( p, furn.id() );
    }
    const auto trap = trap_str_id( o.get_string( "trap" ) );
    if( trap.is_valid() && here.tr_at( p ).loadid != trap.id() ) {
        if( trap.id() == tr_null ) {
            here.remove_trap( p );
        } else {
            here.trap_set( p, trap.id() );
        }
    }
    // fields: replace the set
    auto wanted = std::map<field_type_id, std::pair<int, int>> {};
    auto fa = o.get_array( "f" );
    while( fa.has_more() ) {
        auto one = fa.next_array();
        const auto type = field_type_str_id( one.next_string() );
        const auto intensity = one.next_int();
        const auto age = one.next_int();
        if( type.is_valid() ) {
            wanted[type.id()] = { intensity, age };
        }
    }
    auto to_remove = std::vector<field_type_id> {};
    for( const auto &fld : here.field_at( p ) ) {
        if( fld.second.is_field_alive() && !wanted.contains( fld.first ) ) {
            to_remove.push_back( fld.first );
        }
    }
    for( const auto &type : to_remove ) {
        here.remove_field( p, type );
    }
    for( const auto &[type, val] : wanted ) {
        if( here.get_field_intensity( p, type ) != val.first ) {
            here.remove_field( p, type );
            here.add_field( p, type, val.first, time_duration::from_turns( val.second ), false );
        }
    }
    // items: replace the pile if it differs
    const auto items_json = o.get_string( "i" );
    auto incoming = std::vector<detached_ptr<item>> {};
    {
        auto is = std::istringstream( items_json );
        auto jsin = JsonIn( is );
        jsin.start_array();
        while( !jsin.end_array() ) {
            incoming.push_back( item::spawn( jsin ) );
        }
    }
    auto have = fnv_seed;
    for( const item *it : here.i_at( p ) ) {
        have = item_sig( have, *it );
    }
    auto want = fnv_seed;
    for( const auto &it : incoming ) {
        want = item_sig( want, *it );
    }
    if( have != want ) {
        here.i_clear( p );
        for( auto &it : incoming ) {
            here.add_item( p, std::move( it ) );
        }
    }
}

/// Recompute the stored signature of one tile after a remote change was applied.
auto refresh_sig( const tripoint_abs_ms &abs ) -> void
{
    const auto addr = project_to<coords::sm>( abs );
    const auto it = s().sigs.find( addr );
    if( it == s().sigs.end() ) {
        return;
    }
    auto *sm = MAPBUFFER.get_submap( addr, { .mode = mapbuffer_lookup_mode::resident_only } );
    if( sm == nullptr ) {
        return;
    }
    const auto origin = project_to<coords::ms>( addr );
    const auto l = point_sm_ms( abs.x() - origin.x(), abs.y() - origin.y() );
    it->second[l.x() * SEEY + l.y()] = tile_sig( *sm, l );
}

auto scan_centers() -> std::vector<tripoint_abs_sm>
{
    auto centers = std::vector<tripoint_abs_sm> { project_to<coords::sm>( g->u.abs_pos() ) };
    if( s().peer.in_game && s().peer.has_pos ) {
        centers.push_back( project_to<coords::sm>( s().peer.pos ) );
    }
    return centers;
}

/// Find tiles that changed locally since the last scan and send them to the partner.
auto scan_and_send_tiles() -> void
{
    auto &st = s();
    if( !st.in_game ) {
        return;
    }
    const auto sending = net::connected() && st.peer.in_game;
    auto visited = std::set<tripoint_abs_sm> {};
    auto os = std::ostringstream{};
    auto jo = JsonOut( os );
    auto count = 0;
    jo.start_object();
    jo.member( "t", "tiles" );
    jo.member( "d" );
    jo.start_array();
    for( const auto &center : scan_centers() ) {
        for( auto dz = -1; dz <= 1; ++dz ) {
            for( auto dx = -scan_radius; dx <= scan_radius; ++dx ) {
                for( auto dy = -scan_radius; dy <= scan_radius; ++dy ) {
                    const auto addr = center + tripoint( dx, dy, dz );
                    if( addr.z() < -OVERMAP_DEPTH || addr.z() > OVERMAP_HEIGHT ||
                        !visited.insert( addr ).second ) {
                        continue;
                    }
                    auto *sm = MAPBUFFER.get_submap( addr, { .mode = mapbuffer_lookup_mode::resident_only } );
                    if( sm == nullptr ) {
                        continue;
                    }
                    const auto known = st.sigs.find( addr );
                    if( known == st.sigs.end() ) {
                        // first sight: baseline only, both sides loaded the same data
                        auto &fresh = st.sigs[addr];
                        for( auto x = 0; x < SEEX; ++x ) {
                            for( auto y = 0; y < SEEY; ++y ) {
                                fresh[x * SEEY + y] = tile_sig( *sm, point_sm_ms( x, y ) );
                            }
                        }
                        continue;
                    }
                    for( auto x = 0; x < SEEX; ++x ) {
                        for( auto y = 0; y < SEEY; ++y ) {
                            const auto l = point_sm_ms( x, y );
                            const auto sig = tile_sig( *sm, l );
                            auto &old = known->second[x * SEEY + y];
                            if( sig == old ) {
                                continue;
                            }
                            old = sig;
                            if( sending ) {
                                const auto abs = project_to<coords::ms>( addr ) + tripoint( x, y, 0 );
                                write_tile( jo, abs, *sm, l );
                                ++count;
                            }
                        }
                    }
                }
            }
        }
    }
    jo.end_array();
    jo.end_object();
    if( count > 0 ) {
        net::send( os.str() );
    }
    // forget far away submaps
    if( st.sigs.size() > 4000 ) {
        std::erase_if( st.sigs, [&visited]( const auto & e ) { return !visited.contains( e.first ); } );
    }
}

// --- monsters --------------------------------------------------------------

auto monster_sig( const monster &m ) -> uint64_t
{
    auto h = fnv_seed;
    const auto p = m.abs_pos();
    h = fnv( h, static_cast<uint64_t>( p.x() ) );
    h = fnv( h, static_cast<uint64_t>( p.y() ) );
    h = fnv( h, static_cast<uint64_t>( p.z() ) );
    h = fnv( h, static_cast<uint64_t>( m.get_hp() ) );
    h = fnv( h, static_cast<uint64_t>( m.friendly ) );
    h = fnv( h, static_cast<uint64_t>( m.anger ) );
    h = fnv( h, m.is_dead() ? 1u : 0u );
    h = fnv_str( h, m.type->id.str() );
    return h;
}

auto monster_json( const monster &m ) -> std::string
{
    auto os = std::ostringstream{};
    auto jo = JsonOut( os );
    m.serialize( jo );
    return os.str();
}

auto host_mon_id( monster &m ) -> int
{
    auto &st = s();
    const auto it = st.mon_ids.find( &m );
    if( it != st.mon_ids.end() && it->second.first.lock().get() == &m ) {
        return it->second.second;
    }
    const auto id = st.next_mon_id++;
    const auto ptr = g->shared_from( m );
    st.mon_ids[&m] = { ptr, id };
    st.mon_by_id[id] = ptr;
    return id;
}

auto host_send_monsters( const bool full ) -> void
{
    auto &st = s();
    if( !net::connected() || !st.peer.in_game ) {
        return;
    }
    auto w = msg_writer( "mon" );
    auto alive = std::set<int> {};
    auto count = 0;
    w.jo.member( "up" );
    w.jo.start_array();
    for( monster &m : g->all_monsters() ) {
        if( m.is_dead() ) {
            continue;
        }
        const auto id = host_mon_id( m );
        alive.insert( id );
        const auto sig = monster_sig( m );
        const auto old = st.mon_sig.find( id );
        if( !full && old != st.mon_sig.end() && old->second == sig ) {
            continue;
        }
        st.mon_sig[id] = sig;
        w.jo.start_object();
        w.jo.member( "id", id );
        write_pos( w.jo, "pos", m.abs_pos() );
        w.jo.member( "js", monster_json( m ) );
        w.jo.end_object();
        ++count;
    }
    w.jo.end_array();
    w.jo.member( "rm" );
    w.jo.start_array();
    for( auto it = st.mon_sig.begin(); it != st.mon_sig.end(); ) {
        if( !alive.contains( it->first ) ) {
            w.jo.write( it->first );
            st.mon_by_id.erase( it->first );
            it = st.mon_sig.erase( it );
            ++count;
        } else {
            ++it;
        }
    }
    w.jo.end_array();
    if( count > 0 ) {
        w.send();
    }
    std::erase_if( st.mon_ids, []( const auto & e ) { return e.second.first.expired(); } );
}

auto apply_monster_json( monster &m, const tripoint_abs_ms &pos, const std::string &js ) -> void
{
    auto &tracker = m.get_mapbuffer().creature_tracker();
    if( m.abs_pos() != pos ) {
        tracker.update_pos( m, pos );
    }
    auto is = std::istringstream( js );
    auto jsin = JsonIn( is );
    m.deserialize( jsin );
}

/// A monster can leave the local map (it is stored in its submap) while a weak
/// pointer to it lives on; only monsters on the map may be removed.
auto is_tracked( const monster &m ) -> bool
{
    return g->critter_at<monster>( m.abs_pos(), true ) == &m;
}

/// Client: free @p pos for the monster @p mover. Monsters often swap or follow each other
/// within one host turn; the one in the way gets its own update later in the same message
/// (or no longer exists on the host) and is created again from that update.
auto client_displace_monster( const tripoint_abs_ms &pos, const monster *mover ) -> void
{
    auto &st = s();
    auto *other = g->critter_at<monster>( pos );
    if( other == nullptr || other == mover ) {
        return;
    }
    std::erase_if( st.mon_by_id, [other]( const auto & e ) { return e.second.lock().get() == other; } );
    g->remove_zombie( *other );
}

auto client_apply_monsters( const JsonObject &o ) -> void
{
    auto &st = s();
    auto up = o.get_array( "up" );
    while( up.has_more() ) {
        auto one = up.next_object();
        const auto id = one.get_int( "id" );
        const auto pos = read_pos( one, "pos" );
        const auto js = one.get_string( "js" );
        auto existing = st.mon_by_id.contains( id ) ? st.mon_by_id[id].lock() : nullptr;
        if( existing && ( existing->is_dead() || !is_tracked( *existing ) ) ) {
            existing = nullptr;
        }
        client_displace_monster( pos, existing.get() );
        if( existing ) {
            apply_monster_json( *existing, pos, js );
            st.mon_sig[id] = monster_sig( *existing );
            continue;
        }
        auto fresh = make_shared_fast<monster>();
        auto is = std::istringstream( js );
        auto jsin = JsonIn( is );
        fresh->deserialize( jsin );
        if( fresh->type == nullptr || fresh->is_dead() ) {
            continue;
        }
        fresh->get_mapbuffer().creature_tracker().add( fresh );
        st.mon_by_id[id] = fresh;
        st.mon_sig[id] = monster_sig( *fresh );
    }
    auto rm = o.get_array( "rm" );
    while( rm.has_more() ) {
        const auto id = rm.next_int();
        if( const auto it = st.mon_by_id.find( id ); it != st.mon_by_id.end() ) {
            if( const auto ptr = it->second.lock(); ptr && is_tracked( *ptr ) ) {
                g->remove_zombie( *ptr );
            }
            st.mon_by_id.erase( it );
        }
        st.mon_sig.erase( id );
    }
}

/// Client: report monsters that the local player changed (hit, killed, pushed).
auto client_send_monster_changes() -> void
{
    auto &st = s();
    if( !net::connected() ) {
        return;
    }
    auto w = msg_writer( "monc" );
    auto count = 0;
    auto dead = std::vector<int> {};
    w.jo.member( "up" );
    w.jo.start_array();
    for( auto it = st.mon_by_id.begin(); it != st.mon_by_id.end(); ) {
        const auto ptr = it->second.lock();
        if( !ptr ) {
            st.mon_sig.erase( it->first );
            it = st.mon_by_id.erase( it );
            continue;
        }
        if( ptr->is_dead() ) {
            dead.push_back( it->first );
            st.mon_sig.erase( it->first );
            it = st.mon_by_id.erase( it );
            ++count;
            continue;
        }
        const auto sig = monster_sig( *ptr );
        if( st.mon_sig[it->first] != sig ) {
            st.mon_sig[it->first] = sig;
            w.jo.start_object();
            w.jo.member( "id", it->first );
            write_pos( w.jo, "pos", ptr->abs_pos() );
            w.jo.member( "js", monster_json( *ptr ) );
            w.jo.end_object();
            ++count;
        }
        ++it;
    }
    w.jo.end_array();
    w.jo.member( "dead", dead );
    if( count > 0 ) {
        w.send();
    }
}

auto host_apply_monster_changes( const JsonObject &o ) -> void
{
    auto &st = s();
    auto up = o.get_array( "up" );
    while( up.has_more() ) {
        auto one = up.next_object();
        const auto id = one.get_int( "id" );
        const auto it = st.mon_by_id.find( id );
        const auto ptr = it != st.mon_by_id.end() ? it->second.lock() : nullptr;
        if( !ptr || ptr->is_dead() ) {
            continue;
        }
        const auto pos = read_pos( one, "pos" );
        if( const auto *other = g->critter_at<monster>( pos ); other != nullptr && other != ptr.get() ) {
            // the tile was taken on the host in the meantime; the next host update corrects the client
            st.mon_sig.erase( id );
            continue;
        }
        apply_monster_json( *ptr, pos, one.get_string( "js" ) );
        st.mon_sig[id] = monster_sig( *ptr );
    }
    auto dead = o.get_array( "dead" );
    while( dead.has_more() ) {
        const auto id = dead.next_int();
        const auto it = st.mon_by_id.find( id );
        if( it == st.mon_by_id.end() ) {
            continue;
        }
        if( const auto ptr = it->second.lock(); ptr && is_tracked( *ptr ) ) {
            // the client already dropped the corpse and loot on its side
            g->remove_zombie( *ptr );
        }
        st.mon_by_id.erase( it );
        st.mon_sig.erase( id );
    }
}

auto partner_present() -> bool;

// --- sounds ------------------------------------------------------------------
//
// Each machine only makes the sounds of what it simulates: the host its world,
// the client its own character. The other machine plays them again, so the
// players hear each other and the host's monsters hear the client.

/// At most this many sounds go out at once; the rest are dropped (a horde groaning).
constexpr auto max_sounds_per_send = 200;

/// The local player's shouts are described as "yourself …"; the partner hears
/// them as someone else's.
auto describe_for_partner( const std::string &desc ) -> std::string
{
    if( desc == _( "yourself let out an ear-piercing howl!" ) ) {
        return _( "an ear-piercing howl!" );
    }
    if( desc == _( "yourself scream loudly!" ) ) {
        return _( "a loud scream!" );
    }
    if( desc == _( "yourself shout loudly!" ) ) {
        return _( "a loud shout!" );
    }
    constexpr auto mark = "\x01";
    const auto pattern = string_format( _( "yourself yell \"%s\"" ), mark );
    const auto at = pattern.find( mark );
    if( at != std::string::npos ) {
        const auto prefix = pattern.substr( 0, at );
        const auto suffix = pattern.substr( at + 1 );
        if( desc.size() >= prefix.size() + suffix.size() && desc.starts_with( prefix ) &&
            desc.ends_with( suffix ) ) {
            const auto said = desc.substr( prefix.size(), desc.size() - prefix.size() - suffix.size() );
            return string_format( "%s %s", g->u.name, string_format( _( "yell \"%s\"" ), said ) );
        }
    }
    return desc;
}

auto flush_sounds() -> void
{
    auto &st = s();
    if( st.sounds.empty() ) {
        return;
    }
    if( !partner_present() ) {
        st.sounds.clear();
        return;
    }
    auto w = msg_writer( "snd" );
    w.jo.member( "l" );
    w.jo.start_array();
    auto count = 0;
    for( const auto &r : st.sounds ) {
        if( count++ >= max_sounds_per_send ) {
            break;
        }
        const auto &se = r.se;
        w.jo.start_object();
        write_pos( w.jo, "p", r.pos );
        w.jo.member( "v", static_cast<int>( se.volume ) );
        w.jo.member( "c", static_cast<int>( se.category ) );
        w.jo.member( "d", se.from_player ? describe_for_partner( se.description ) : se.description );
        w.jo.member( "mv", se.movement_noise );
        // a sound of the local player is a sound of the partner over there
        w.jo.member( "src", se.from_player ? "p" : se.from_npc ? "n" : se.from_monster ? "m" : "" );
        w.jo.member( "id", se.id );
        w.jo.member( "var", se.variant );
        w.jo.member( "f", se.faction.str() );
        w.jo.member( "mf", se.monfaction.str() );
        w.jo.end_object();
    }
    w.jo.end_array();
    w.send();
    st.sounds.clear();
}

auto apply_sounds( const JsonObject &o ) -> void
{
    auto &st = s();
    for( auto one : o.get_array( "l" ) ) {
        auto r = one.get_object();
        r.allow_omitted_members();
        const auto pos = abs_to_bub( read_pos( r, "p" ) );
        if( !g->m.inbounds( pos ) ) {
            continue;
        }
        auto se = sound_event{};
        se.origin = pos;
        se.volume = static_cast<short>( r.get_int( "v" ) );
        se.category = static_cast<sounds::sound_t>( std::clamp( r.get_int( "c" ), 0,
                      static_cast<int>( sounds::sound_t::_LAST ) - 1 ) );
        se.description = r.get_string( "d" );
        se.movement_noise = r.get_bool( "mv" );
        se.id = r.get_string( "id" );
        se.variant = r.get_string( "var" );
        const auto src = r.get_string( "src" );
        if( src == "p" ) {
            // the partner is a friendly character here
            se.from_npc = true;
            se.faction = faction_id( "your_followers" );
        } else if( src == "n" ) {
            se.from_npc = true;
            se.faction = faction_id( r.get_string( "f" ) );
        } else if( src == "m" ) {
            se.from_monster = true;
            se.monfaction = mfaction_str_id( r.get_string( "mf" ) );
        }
        if( se.description.empty() ) {
            continue;
        }
        st.playing_partner_sound = true;
        sounds::sound( se );
        st.playing_partner_sound = false;
    }
}

// --- vehicles ----------------------------------------------------------------
//
// The host runs vehicle physics. Every vehicle gets a co-op id (a tag) the
// first time the host sends it. The host sends motion ("vmove") and, when parts
// or cargo change, the whole vehicle ("vfull"). The client sends "vfull" when
// its player changes a vehicle (loot, repairs, doors).

constexpr auto veh_tag_prefix = std::string_view{ "coop_veh_" };
/// Fuel levels change all the time while driving; they are re-sent at most this often.
constexpr auto veh_fuel_interval = 30;

auto veh_id_of( const vehicle &v ) -> std::string
{
    for( const auto &tag : v.tags ) {
        if( tag.starts_with( veh_tag_prefix ) ) {
            return tag.substr( veh_tag_prefix.size() );
        }
    }
    return {};
}

auto set_veh_id( vehicle &v, const std::string &id ) -> void
{
    for( auto it = v.tags.begin(); it != v.tags.end(); ) {
        it = it->starts_with( veh_tag_prefix ) ? v.tags.erase( it ) : std::next( it );
    }
    v.tags.insert( std::string( veh_tag_prefix ) + id );
}

auto new_veh_id() -> std::string
{
    return string_format( "%08x%08x", rng( 0, INT_MAX ), rng( 0, INT_MAX ) );
}

auto host_tag_vehicle( vehicle &v ) -> std::string
{
    auto id = veh_id_of( v );
    if( id.empty() ) {
        id = new_veh_id();
        set_veh_id( v, id );
    }
    return id;
}

auto angle_bits( const units::angle a ) -> uint64_t
{
    return static_cast<uint64_t>( std::llround( units::to_degrees( a ) * 100.0 ) );
}

auto veh_motion_sig( const vehicle &v ) -> uint64_t
{
    auto h = fnv_seed;
    const auto p = v.abs_ms_location();
    h = fnv( h, static_cast<uint64_t>( p.x() ) );
    h = fnv( h, static_cast<uint64_t>( p.y() ) );
    h = fnv( h, static_cast<uint64_t>( p.z() ) );
    h = fnv( h, angle_bits( v.face.dir() ) );
    h = fnv( h, angle_bits( v.move.dir() ) );
    h = fnv( h, angle_bits( v.turn_dir ) );
    h = fnv( h, angle_bits( v.pivot_rotation[0] ) );
    h = fnv( h, static_cast<uint64_t>( v.pivot_anchor[0].x() ) );
    h = fnv( h, static_cast<uint64_t>( v.pivot_anchor[0].y() ) );
    h = fnv( h, static_cast<uint64_t>( v.velocity ) );
    h = fnv( h, static_cast<uint64_t>( v.cruise_velocity ) );
    h = fnv( h, v.skidding ? 1u : 0u );
    return h;
}

/// Signature of parts and cargo. Fuel goes to @p fuel: it changes every turn of driving.
auto veh_content_sig( vehicle &v, uint64_t &fuel ) -> uint64_t
{
    auto h = fnv_str( fnv_seed, v.name );
    h = fnv( h, ( v.is_locked ? 1u : 0u ) | ( v.is_alarm_on ? 2u : 0u ) | ( v.camera_on ? 4u : 0u ) |
             ( v.engine_on ? 8u : 0u ) | ( v.cruise_on ? 16u : 0u ) | ( v.brake_hold ? 32u : 0u ) |
             ( v.tracking_on ? 64u : 0u ) | ( v.is_following ? 128u : 0u ) |
             ( v.is_patrolling ? 256u : 0u ) );
    h = fnv( h, static_cast<uint64_t>( v.follow_distance ) );
    h = fnv( h, static_cast<uint64_t>( v.min_autodrive_speed ) );
    h = fnv( h, static_cast<uint64_t>( v.max_autodrive_speed ) );
    h = fnv_str( h, v.get_owner().str() );
    for( const auto &tag : v.tags ) {
        h = fnv_str( h, tag );
    }
    for( const auto &l : v.labels ) {
        h = fnv_str( h, l.text );
        h = fnv( h, static_cast<uint64_t>( l.x() ) );
        h = fnv( h, static_cast<uint64_t>( l.y() ) );
    }
    fuel = fnv_seed;
    for( auto i = 0; i < v.part_count(); ++i ) {
        const auto &p = v.part( i );
        h = fnv_str( h, p.info().get_id().str() );
        h = fnv( h, ( p.removed ? 1u : 0u ) | ( p.enabled ? 2u : 0u ) | ( p.open ? 4u : 0u ) );
        if( p.removed ) {
            continue;
        }
        h = fnv( h, static_cast<uint64_t>( p.hp() ) );
        h = fnv( h, static_cast<uint64_t>( p.mount.x() ) );
        h = fnv( h, static_cast<uint64_t>( p.mount.y() ) );
        fuel = fnv( fuel, static_cast<uint64_t>( p.ammo_remaining() ) );
        for( const item *it : p.get_items() ) {
            h = item_sig( h, *it );
        }
    }
    return h;
}

/// Record what this machine has now as the synced state of a vehicle.
auto remember_vehicle( vehicle &v, const std::string &id ) -> veh_state &
{
    auto &vs = s().vehs[id];
    vs.content = veh_content_sig( v, vs.fuel );
    vs.motion = veh_motion_sig( v );
    vs.pos = v.abs_ms_location();
    return vs;
}

auto write_motion( JsonOut &jo, const vehicle &v ) -> void
{
    jo.member( "sm" );
    jo.start_array();
    jo.write( v.abs_sm_pos.x() );
    jo.write( v.abs_sm_pos.y() );
    jo.write( v.abs_sm_pos.z() );
    jo.end_array();
    jo.member( "x", v.sm_ms_pos.x() );
    jo.member( "y", v.sm_ms_pos.y() );
    jo.member( "rot", units::to_degrees( v.pivot_rotation[0] ) );
    jo.member( "pa" );
    jo.start_array();
    jo.write( v.pivot_anchor[0].x() );
    jo.write( v.pivot_anchor[0].y() );
    jo.write( v.pivot_anchor[0].z() );
    jo.end_array();
    jo.member( "f", units::to_degrees( v.face.dir() ) );
    jo.member( "m", units::to_degrees( v.move.dir() ) );
    jo.member( "td", units::to_degrees( v.turn_dir ) );
    jo.member( "lt", units::to_degrees( v.last_turn ) );
    jo.member( "v", v.velocity );
    jo.member( "cv", v.cruise_velocity );
    jo.member( "sk", v.skidding );
}

auto read_sm( const JsonObject &o ) -> tripoint_abs_sm
{
    auto a = o.get_array( "sm" );
    const auto x = a.next_int();
    const auto y = a.next_int();
    const auto z = a.next_int();
    return tripoint_abs_sm( x, y, z );
}

auto motion_target( const JsonObject &o ) -> tripoint_abs_ms
{
    return project_combine( read_sm( o ), point_sm_ms( o.get_int( "x" ), o.get_int( "y" ) ) );
}

auto send_vehicle( vehicle &v, const std::string &id, const bool full ) -> void
{
    if( full ) {
        DebugLog( DL::Info, DC::Main ) << "coop: send vehicle " << id << " " << v.name;
    }
    auto w = msg_writer( full ? "vfull" : "vmove" );
    w.jo.member( "id", id );
    write_motion( w.jo, v );
    if( full ) {
        auto os = std::ostringstream{};
        {
            auto jo = JsonOut( os );
            v.serialize( jo );
        }
        w.jo.member( "js", os.str() );
    }
    w.send();
}

auto find_vehicle( const std::string &id ) -> vehicle *
{
    for( auto &w : g->m.get_vehicles() ) {
        if( veh_id_of( *w.v ) == id ) {
            return w.v;
        }
    }
    return nullptr;
}

auto local_buffer() -> mapbuffer &
{
    return MAPBUFFER_REGISTRY.get( g->m.get_bound_dimension() );
}

/// Drop an OMT the client keeps in memory outside its map window; it is read
/// from the host again when needed.
auto client_forget_omt( const tripoint_abs_omt &omt ) -> void
{
    const auto base = project_to<coords::sm>( omt );
    for( const auto off : { point_rel_sm::zero(), point_rel_sm::east(), point_rel_sm::south(), point_rel_sm::south_east() } ) {
        for( auto z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; ++z ) {
            if( g->m.inbounds( tripoint_abs_sm( ( base + off ).xy(), z ) ) ) {
                return;
            }
        }
    }
    local_buffer().unload_omt( omt, false );
}

/// The client may keep old copies of a vehicle in submaps outside its map window.
auto client_forget_stale_copies( const std::string &id ) -> void
{
    auto omts = std::set<tripoint_abs_omt>{};
    for( auto &[pos, sm] : local_buffer() ) {
        if( !sm || g->m.inbounds( pos ) ) {
            continue;
        }
        for( const auto &v : sm->vehicles ) {
            if( v && veh_id_of( *v ) == id ) {
                omts.insert( project_to<coords::omt>( pos ) );
            }
        }
    }
    for( const auto &omt : omts ) {
        client_forget_omt( omt );
    }
}

/// Put a vehicle into the submap given by its abs_sm_pos on the local map.
auto place_in_bubble( std::unique_ptr<vehicle> fresh ) -> vehicle *
{
    auto &buf = local_buffer();
    auto *sm = buf.lookup_submap_in_memory( fresh->abs_sm_pos );
    if( sm == nullptr ) {
        return nullptr;
    }
    fresh->set_dimension( g->m.get_bound_dimension() );
    fresh->attach();
    auto *raw = fresh.get();
    sm->vehicles.push_back( std::move( fresh ) );
    sm->is_uniform = false;
    raw->refresh_position();
    g->m.update_vehicle_list( buf.lookup_submap_in_memory( raw->abs_sm_pos ), raw->abs_sm_pos.z() );
    g->m.add_vehicle_to_cache( raw );
    buf.register_vehicle( raw );
    return raw;
}

/// Empty the seats of a vehicle without the usual side effects; returns who sat there.
auto clear_riders( vehicle &v ) -> std::vector<Character *>
{
    auto riders = std::vector<Character *> {};
    for( auto i = 0; i < v.part_count(); ++i ) {
        auto &p = v.part( i );
        if( !p.has_flag( vehicle_part::passenger_flag ) ) {
            continue;
        }
        if( auto *psg = v.get_passenger( i ) ) {
            riders.push_back( psg );
        }
        p.remove_flag( vehicle_part::passenger_flag );
    }
    return riders;
}

auto remove_vehicle( vehicle &v ) -> void
{
    for( auto *who : clear_riders( v ) ) {
        who->in_vehicle = false;
        who->controlling_vehicle = false;
    }
    if( g->remoteveh() == &v ) {
        g->setremoteveh( nullptr );
    }
    g->m.detach_vehicle( &v );
}

/// Swap a vehicle on the local map for a new copy at the same place.
auto replace_in_bubble( vehicle &old, std::unique_ptr<vehicle> fresh ) -> vehicle *
{
    const auto riders = clear_riders( old );
    fresh->abs_sm_pos = old.abs_sm_pos;
    fresh->sm_ms_pos = old.sm_ms_pos;
    const auto was_remote = g->remoteveh() == &old;
    if( was_remote ) {
        g->setremoteveh( nullptr );
    }
    g->m.detach_vehicle( &old );
    auto *placed = place_in_bubble( std::move( fresh ) );
    if( placed == nullptr ) {
        return nullptr;
    }
    for( auto *who : riders ) {
        if( !quiet_board( *who ) ) {
            who->in_vehicle = false;
            who->controlling_vehicle = false;
        }
    }
    if( was_remote ) {
        g->setremoteveh( placed );
    }
    return placed;
}

/// Host: change a vehicle that lies outside the host's map window.
auto host_replace_outside( const tripoint_abs_sm &smpos, const std::string &id,
                           std::unique_ptr<vehicle> fresh ) -> void
{
    auto &buf = local_buffer();
    auto *sm = buf.get_submap( smpos, { .mode = mapbuffer_lookup_mode::load_or_generate } );
    if( sm == nullptr ) {
        return;
    }
    fresh->abs_sm_pos = smpos;
    fresh->set_dimension( g->m.get_bound_dimension() );
    for( auto &slot : sm->vehicles ) {
        if( slot && veh_id_of( *slot ) == id ) {
            buf.unregister_vehicle( slot.get() );
            slot = std::move( fresh );
            buf.register_vehicle( slot.get() );
            return;
        }
    }
    auto *raw = fresh.get();
    sm->vehicles.push_back( std::move( fresh ) );
    sm->is_uniform = false;
    buf.register_vehicle( raw );
}

/// Move and turn a local vehicle the way the host did.
auto apply_motion( vehicle &v, const JsonObject &o ) -> void
{
    const auto target = motion_target( o );
    const auto rot = units::from_degrees( o.get_float( "rot" ) );
    auto pa = o.get_array( "pa" );
    const auto pax = pa.next_int();
    const auto pay = pa.next_int();
    const auto paz = pa.next_int();
    const auto pivot = tripoint_mnt_veh( pax, pay, paz );
    v.face.init( units::from_degrees( o.get_float( "f" ) ) );
    v.move.init( units::from_degrees( o.get_float( "m" ) ) );
    v.turn_dir = units::from_degrees( o.get_float( "td" ) );
    v.last_turn = units::from_degrees( o.get_float( "lt" ) );
    v.velocity = o.get_int( "v" );
    v.cruise_velocity = o.get_int( "cv" );
    v.skidding = o.get_bool( "sk" );
    const auto dp = target - v.abs_ms_location();
    if( dp == tripoint_rel_ms::zero() && rot == v.pivot_rotation[0] && pivot == v.pivot_anchor[0] ) {
        return;
    }
    v.precalc_mounts( 1, rot, pivot );
    if( dp.z() == 0 ) {
        v.adjust_zlevel( 1, dp );
    }
    g->m.displace_vehicle( v, dp );
    v.shift_zlevel();
    g->invalidate_main_ui_adaptor();
}

auto client_request_vehicle( const std::string &id ) -> void
{
    auto &st = s();
    auto &when = st.veh_requested[id];
    if( when != 0 && my_turn() - when < 10 ) {
        return;
    }
    when = my_turn();
    auto w = msg_writer( "vreq" );
    w.jo.member( "id", id );
    w.send();
}

auto apply_vmove( const JsonObject &o ) -> void
{
    auto &st = s();
    const auto id = o.get_string( "id" );
    const auto target = motion_target( o );
    auto *v = find_vehicle( id );
    if( v == nullptr ) {
        if( g->m.inbounds( target ) ) {
            client_request_vehicle( id );
        }
        return;
    }
    if( !g->m.inbounds( target ) ) {
        // the vehicle leaves the local map
        remove_vehicle( *v );
        st.vehs.erase( id );
        client_forget_omt( project_to<coords::omt>( target ) );
        return;
    }
    apply_motion( *v, o );
    // only the motion is synced now: a change the local player just made to
    // the vehicle must still go out
    auto &vs = st.vehs[id];
    vs.motion = veh_motion_sig( *v );
    vs.pos = v->abs_ms_location();
}

auto apply_vfull( const JsonObject &o ) -> void
{
    auto &st = s();
    const auto id = o.get_string( "id" );
    DebugLog( DL::Info, DC::Main ) << "coop: got vehicle " << id;
    const auto smpos = read_sm( o );
    const auto target = motion_target( o );
    auto fresh = std::make_unique<vehicle>();
    try {
        auto is = std::istringstream( o.get_string( "js" ) );
        auto jsin = JsonIn( is );
        fresh->deserialize( jsin );
    } catch( const std::exception &err ) {
        DebugLog( DL::Error, DC::Main ) << "coop: bad vehicle " << id << ": " << err.what();
        return;
    }
    set_veh_id( *fresh, id );
    // seats are taken by local characters only
    for( auto i = 0; i < fresh->part_count(); ++i ) {
        fresh->part( i ).remove_flag( vehicle_part::passenger_flag );
        fresh->part( i ).passenger_id = character_id();
    }
    st.veh_requested.erase( id );
    auto *old = find_vehicle( id );
    if( old != nullptr ) {
        if( is_host() ) {
            // the host's physics decide where the vehicle is
            fresh->face = old->face;
            fresh->move = old->move;
            fresh->turn_dir = old->turn_dir;
            fresh->last_turn = old->last_turn;
            fresh->velocity = old->velocity;
            fresh->cruise_velocity = old->cruise_velocity;
            fresh->vertical_velocity = old->vertical_velocity;
            fresh->skidding = old->skidding;
            fresh->pivot_anchor = old->pivot_anchor;
            fresh->pivot_rotation = old->pivot_rotation;
        } else if( g->m.inbounds( target ) ) {
            apply_motion( *old, o );
        }
        if( auto *placed = replace_in_bubble( *old, std::move( fresh ) ) ) {
            remember_vehicle( *placed, id );
        }
        return;
    }
    if( g->m.inbounds( target ) ) {
        if( is_client() ) {
            client_forget_stale_copies( id );
        }
        fresh->abs_sm_pos = smpos;
        if( auto *placed = place_in_bubble( std::move( fresh ) ) ) {
            remember_vehicle( *placed, id );
            for( auto &guy : g->all_npcs() ) {
                if( is_puppet( guy ) && guy.in_vehicle ) {
                    quiet_board( guy );
                }
            }
        }
    } else if( is_host() ) {
        host_replace_outside( smpos, id, std::move( fresh ) );
    } else {
        client_forget_stale_copies( id );
    }
}

auto apply_vgone( const JsonObject &o ) -> void
{
    const auto id = o.get_string( "id" );
    if( auto *v = find_vehicle( id ) ) {
        remove_vehicle( *v );
    }
    s().vehs.erase( id );
}

/// Drop seat marks of characters who are not in that seat. Map chunks carry the
/// other machine's passengers, and their ids mean someone else here.
auto clear_stale_seats( vehicle &v ) -> void
{
    for( auto i = 0; i < v.part_count(); ++i ) {
        auto &p = v.part( i );
        if( !p.has_flag( vehicle_part::passenger_flag ) ) {
            continue;
        }
        const auto *psg = v.get_passenger( i );
        if( psg == nullptr || !psg->in_vehicle || psg->bub_pos() != v.bub_part_location( i ) ) {
            p.remove_flag( vehicle_part::passenger_flag );
            v.invalidate_mass();
        }
    }
}

/// Send what changed about the vehicles on the local map.
auto sync_vehicles( const bool full ) -> void
{
    auto &st = s();
    if( !partner_present() ) {
        return;
    }
    const auto turn = my_turn();
    auto seen = std::set<std::string> {};
    for( auto &w : g->m.get_vehicles() ) {
        auto &v = *w.v;
        clear_stale_seats( v );
        auto id = is_host() ? host_tag_vehicle( v ) : veh_id_of( v );
        if( id.empty() ) {
            continue;
        }
        if( seen.contains( id ) ) {
            if( !is_host() ) {
                continue;
            }
            // a vehicle broke in two: the new piece gets its own id
            id = new_veh_id();
            set_veh_id( v, id );
        }
        seen.insert( id );
        auto fuel = uint64_t{ 0 };
        const auto content = veh_content_sig( v, fuel );
        const auto known = st.vehs.find( id );
        if( known == st.vehs.end() && !is_host() ) {
            // the client got it with a map chunk
            remember_vehicle( v, id );
            continue;
        }
        auto &vs = known == st.vehs.end() ? st.vehs[id] : known->second;
        const auto fresh = known == st.vehs.end();
        const auto send_full = full || fresh || content != vs.content ||
                               ( fuel != vs.fuel && turn - vs.full_turn >= veh_fuel_interval );
        if( send_full ) {
            send_vehicle( v, id, true );
            remember_vehicle( v, id ).full_turn = turn;
        } else if( is_host() && veh_motion_sig( v ) != vs.motion ) {
            send_vehicle( v, id, false );
            remember_vehicle( v, id );
        } else {
            vs.pos = v.abs_ms_location();
        }
    }
    // a vehicle that vanished inside the map window was destroyed
    for( auto it = st.vehs.begin(); it != st.vehs.end(); ) {
        if( seen.contains( it->first ) ) {
            ++it;
            continue;
        }
        if( g->m.inbounds( it->second.pos ) ) {
            auto w = msg_writer( "vgone" );
            w.jo.member( "id", it->first );
            w.send();
        }
        it = st.vehs.erase( it );
    }
}

/// Host: the partner steers or speeds up a vehicle.
auto host_handle_drive( const JsonObject &o ) -> void
{
    auto &st = s();
    auto *v = find_vehicle( o.get_string( "id" ) );
    if( v == nullptr || !st.puppet || !st.puppet->in_vehicle ) {
        return;
    }
    const auto vp = g->m.veh_at( st.puppet->bub_pos() );
    if( !vp || &vp->vehicle() != v ) {
        return;
    }
    st.puppet->controlling_vehicle = true;
    v->pldrive( *st.puppet, tripoint_rel_veh( o.get_int( "x" ), o.get_int( "y" ), o.get_int( "z" ) ) );
    st.puppet->moves = 0;
}

auto host_handle_vreq( const JsonObject &o ) -> void
{
    const auto id = o.get_string( "id" );
    if( auto *v = find_vehicle( id ) ) {
        send_vehicle( *v, id, true );
        remember_vehicle( *v, id ).full_turn = my_turn();
    }
}

// --- dialogue and missions -------------------------------------------------

auto partner_present() -> bool;
auto hud_banner( const std::string &text ) -> void;

auto excl_key( const int uid ) -> std::string
{
    return "coop_only_" + std::to_string( uid );
}

/// Missions in which only one of the players does something for himself: bring items
/// or kill a number of monsters, with no place in the world tied to them. Each player
/// can take such a mission for himself. All other missions are for the team: once
/// taken by one player, the NPC no longer offers them.
auto is_personal( const mission &m ) -> bool
{
    switch( m.get_type().goal ) {
        case MGOAL_FIND_ITEM:
        case MGOAL_FIND_ITEM_GROUP:
        case MGOAL_KILL_MONSTER_TYPE:
        case MGOAL_KILL_MONSTER_SPEC:
            return !m.has_target();
        default:
            return false;
    }
}

auto mission_to_json( const mission &m ) -> std::string
{
    auto os = std::ostringstream{};
    auto jo = JsonOut( os );
    m.serialize( jo );
    return os.str();
}

auto mission_from_json( const std::string &js ) -> mission
{
    auto res = mission{};
    auto is = std::istringstream( js );
    auto jsin = JsonIn( is );
    res.deserialize( jsin );
    return res;
}

auto npc_to_json( const npc &guy ) -> std::string
{
    auto os = std::ostringstream{};
    auto jo = JsonOut( os );
    guy.serialize( jo );
    return os.str();
}

auto npc_from_json( npc &guy, const std::string &js ) -> void
{
    // loading sets the id again; it is the same id. The dimension stays the local one:
    // creatures in different dimensions do not see each other.
    const auto id = guy.getID();
    const auto dimension = guy.get_dimension();
    guy.setID( character_id(), true );
    auto is = std::istringstream( js );
    auto jsin = JsonIn( is );
    try {
        guy.deserialize( jsin );
    } catch( const std::exception &err ) {
        DebugLog( DL::Error, DC::Main ) << "coop: bad NPC data: " << err.what();
    }
    if( !guy.getID().is_valid() ) {
        guy.setID( id, true );
    }
    guy.set_dimension( dimension );
}

/// Host: what the NPC thinks of the partner. The NPC's own opinion is about the host.
auto partner_opinion( const npc &guy ) -> npc_opinion
{
    auto res = npc_opinion{};
    auto in = std::istringstream( guy.get_value( "coop_partner_opinion" ) );
    in >> res.trust >> res.fear >> res.value >> res.anger >> res.owed;
    return in ? res : npc_opinion{};
}

auto set_partner_opinion( npc &guy, const npc_opinion &op ) -> void
{
    guy.set_value( "coop_partner_opinion", string_format( "%d %d %d %d %d", op.trust, op.fear,
                   op.value, op.anger, op.owed ) );
}

auto kind_key( const int uid ) -> std::string
{
    return "coop_kind_" + std::to_string( uid );
}

/// Host: a team mission is shared: both players have it, either can finish it.
/// The decision is taken when the mission is given out and kept on the giver;
/// missions given out before that existed are judged by their goal.
auto is_team( const mission &m ) -> bool
{
    if( !m.get_npc_id().is_valid() ) {
        return false;
    }
    const auto *giver = g->find_npc( m.get_npc_id() );
    if( giver == nullptr ) {
        return false;
    }
    const auto kind = giver->get_value( kind_key( m.get_id() ) );
    return kind.empty() ? !is_personal( m ) : kind == "team";
}

auto record_kind( const mission &m ) -> void
{
    if( auto *giver = g->find_npc( m.get_npc_id() ) ) {
        giver->set_value( kind_key( m.get_id() ), is_personal( m ) ? "personal" : "team" );
    }
}

/// Host: mission ids as the partner's machine knows them.
auto mission_for_partner( const mission &m ) -> std::string
{
    auto copy = m;
    const auto who = coop_mission_access::player( m );
    if( who.is_valid() && is_team( m ) ) {
        // on each machine a team mission belongs to the local player
        coop_mission_access::set_player( copy, s().partner_avatar );
    } else if( who == g->u.getID() ) {
        coop_mission_access::set_player( copy, host_stand_in_id );
    } else if( who == partner_id ) {
        coop_mission_access::set_player( copy, s().partner_avatar );
    }
    return mission_to_json( copy );
}

/// Host: take a mission back from the partner's machine.
auto mission_from_partner( const std::string &js, const bool mine ) -> void
{
    auto m = mission_from_json( js );
    const auto who = coop_mission_access::player( m );
    if( who.is_valid() && is_team( m ) ) {
        coop_mission_access::set_player( m, g->u.getID() );
    } else if( mine ) {
        coop_mission_access::set_player( m, partner_id );
    } else if( who == host_stand_in_id ) {
        coop_mission_access::set_player( m, g->u.getID() );
    }
    mission::add_existing( m );
}

auto in_list( const std::vector<mission *> &list, const mission *m ) -> bool
{
    return std::ranges::find( list, m ) != list.end();
}

/// Host: give a team mission to the partner as well.
auto share_team_mission( const mission &m ) -> void
{
    if( !net::connected() || !s().peer.in_game ) {
        return;
    }
    auto w = msg_writer( "mteam" );
    w.jo.member( "js", mission_for_partner( m ) );
    w.send();
}

/// Host: all running team missions, for a partner who joins.
auto share_all_team_missions() -> void
{
    for( const mission *m : mission::get_all_active() ) {
        if( m->in_progress() && coop_mission_access::player( *m ) == g->u.getID() && is_team( *m ) ) {
            share_team_mission( *m );
        }
    }
}

/// Client: a team mission from the host.
auto client_take_team_mission( const std::string &js ) -> void
{
    const auto incoming = mission_from_json( js );
    mission::add_existing( incoming );
    auto *m = mission::find( incoming.get_id() );
    if( m == nullptr || !m->in_progress() || in_list( g->u.get_active_missions(), m ) ) {
        return;
    }
    coop_mission_access::finish_on( *m, g->u );
    add_msg( m_info, _( "Team mission: %s." ), m->name() );
}

/// Either side: a team mission ended on the other machine; close it here too.
auto close_finished_mission( mission &m, const std::string &who ) -> void
{
    if( !m.in_progress() && in_list( g->u.get_active_missions(), &m ) ) {
        g->u.on_mission_finished( m );
        if( !m.has_failed() ) {
            add_msg( m_good, _( "%1$s completed the team mission \"%2$s\"." ), who, m.name() );
        }
    }
}

/// Missions offered to the other player only, taken out for the time of a dialogue.
auto hide_missions_for( npc &guy, const std::string &other ) -> std::vector<mission *>
{
    auto hidden = std::vector<mission *> {};
    std::erase_if( guy.chatbin.missions, [&]( mission * m ) {
        if( m != nullptr && guy.get_value( excl_key( m->get_id() ) ) == other ) {
            hidden.push_back( m );
            return true;
        }
        return false;
    } );
    return hidden;
}

/// After a dialogue: put hidden missions back and add copies made during it.
auto restore_missions( npc &guy, const std::vector<mission *> &hidden ) -> void
{
    auto &st = s();
    for( mission *m : hidden ) {
        guy.chatbin.missions.push_back( m );
    }
    std::erase_if( st.pending_copies, [&guy]( const auto & entry ) {
        auto *copy = mission::find( entry.first );
        if( copy == nullptr ) {
            return true;
        }
        if( copy->get_npc_id() != guy.getID() ) {
            return false;
        }
        guy.chatbin.add_new_mission( copy );
        guy.set_value( excl_key( entry.first ), entry.second );
        return true;
    } );
}

/// Every mission that the NPC gives or gave.
auto missions_of( const npc &guy ) -> std::set<int>
{
    auto res = std::set<int> {};
    for( const mission *m : guy.chatbin.missions ) {
        res.insert( m->get_id() );
    }
    for( const mission *m : guy.chatbin.missions_assigned ) {
        res.insert( m->get_id() );
    }
    for( const mission *m : mission::get_all_active() ) {
        if( m->get_npc_id() == guy.getID() ) {
            res.insert( m->get_id() );
        }
    }
    return res;
}

/// Client: wait for the host's answer without freezing the window.
auto wait_reply( const std::string &key, const int timeout_ms ) -> std::optional<std::string>
{
    const auto start = clock_type::now();
    auto shown = false;
    while( partner_present() && ms_since( start ) < timeout_ms ) {
        if( auto data = net::take_blob( key, -1 ) ) {
            return data;
        }
        if( !shown && ms_since( start ) > 300 ) {
            hud_banner( _( "waiting for the host…" ) );
            shown = true;
        }
        inp_mngr.pump_events();
        std::this_thread::sleep_for( std::chrono::milliseconds( 15 ) );
    }
    return std::nullopt;
}

auto host_handle_talk_req( const JsonObject &o ) -> void
{
    auto &st = s();
    const auto key = o.get_string( "req" );
    st.partner_avatar = character_id( o.get_int( "me" ) );
    auto out = std::ostringstream{};
    auto jo = JsonOut( out );
    jo.start_object();
    auto *guy = g->find_npc( character_id( o.get_int( "id" ) ) );
    if( guy == nullptr || guy->is_dead() ) {
        jo.member( "ok", false );
        jo.member( "why", _( "They are too far from the host." ) );
    } else if( st.host_talking == guy->getID() ) {
        jo.member( "ok", false );
        jo.member( "why", string_format( _( "%1$s is talking to %2$s." ), guy->disp_name(), g->u.name ) );
    } else {
        st.talking_npc = guy->getID();
        if( guy->get_value( "coop_partner_opinion" ).empty() && st.puppet ) {
            // the first meeting: the NPC sizes the partner up the same way it did the host
            // when it appeared (weapon, armor, looks). A private copy of the NPC does it,
            // so the real NPC keeps its attitude and its opinion of the host.
            auto judge = make_shared_fast<npc>();
            judge->set_fake( true );
            npc_from_json( *judge, npc_to_json( *guy ) );
            judge->set_fake( true );
            judge->op_of_u = npc_opinion{};
            judge->form_opinion( *st.puppet );
            set_partner_opinion( *guy, judge->op_of_u );
        }
        jo.member( "ok", true );
        jo.member( "missions" );
        jo.start_array();
        for( const auto uid : missions_of( *guy ) ) {
            if( const auto *m = mission::find( uid ) ) {
                jo.write( mission_for_partner( *m ) );
            }
        }
        jo.end_array();
        const auto own = guy->op_of_u;
        guy->op_of_u = partner_opinion( *guy );
        jo.member( "npc", npc_to_json( *guy ) );
        guy->op_of_u = own;
    }
    jo.end_object();
    net::send_blob( key, out.str() );
}

auto host_handle_talk_done( const JsonObject &o ) -> void
{
    auto &st = s();
    const auto id = character_id( o.get_int( "id" ) );
    if( st.talking_npc == id ) {
        st.talking_npc = character_id();
    }
    auto *guy = g->find_npc( id );
    if( guy == nullptr ) {
        return;
    }
    auto missions = o.get_array( "missions" );
    while( missions.has_more() ) {
        auto one = missions.next_object();
        const auto incoming = mission_from_json( one.get_string( "js" ) );
        mission_from_partner( one.get_string( "js" ), one.get_bool( "mine" ) );
        if( auto *m = mission::find( incoming.get_id() ) ) {
            close_finished_mission( *m, st.peer.name );
        }
    }
    const auto own = guy->op_of_u;
    const auto pos = guy->abs_pos();
    npc_from_json( *guy, o.get_string( "npc" ) );
    set_partner_opinion( *guy, guy->op_of_u );
    guy->op_of_u = own;
    // loading wrote the position directly; this puts the NPC back into the game's
    // index of NPC positions, where it stays at its host-side place
    guy->setpos( pos );
    // the partner gets the new look with the next update
    st.npc_sig.erase( id.get_value() );
}

auto host_handle_mission_new( const JsonObject &o ) -> void
{
    auto *m = mission::reserve_new( mission_type_id( o.get_string( "type" ) ),
                                    character_id( o.get_int( "npc" ) ) );
    net::send_blob( o.get_string( "req" ), m != nullptr ? mission_to_json( *m ) : std::string() );
}

auto host_handle_mission_assign( const JsonObject &o ) -> void
{
    auto &st = s();
    st.partner_avatar = character_id( o.get_int( "me" ) );
    auto out = std::ostringstream{};
    auto jo = JsonOut( out );
    jo.start_object();
    auto *m = mission::find( o.get_int( "uid" ) );
    if( m == nullptr || m->is_assigned() ) {
        jo.member( "ok", false );
    } else {
        coop_mission_access::start_for( *m, partner_id );
        record_kind( *m );
        if( is_team( *m ) ) {
            // the host has it too
            coop_mission_access::set_player( *m, g->u.getID() );
            g->u.on_mission_assignment( *m );
            add_msg( m_info, _( "%1$s took the team mission \"%2$s\"." ), st.peer.name, m->name() );
        }
        jo.member( "ok", true );
        jo.member( "mission", mission_for_partner( *m ) );
        // a personal mission: the host may take the same one for himself
        const auto *giver = g->find_npc( m->get_npc_id() );
        if( is_personal( *m ) && giver != nullptr && giver->get_value( excl_key( m->get_id() ) ).empty() ) {
            if( auto *copy = mission::reserve_new( m->get_type().id, m->get_npc_id() ) ) {
                jo.member( "copy", mission_to_json( *copy ) );
            }
        }
    }
    jo.end_object();
    net::send_blob( o.get_string( "req" ), out.str() );
}

auto client_talk( npc &guy, const bool radio_contact, const bool enforce_first_topic ) -> void
{
    auto &st = s();
    const auto key = string_format( "talk:%d", ++st.req_counter );
    {
        auto w = msg_writer( "talk_req" );
        w.jo.member( "req", key );
        w.jo.member( "id", guy.getID().get_value() );
        w.jo.member( "me", g->u.getID().get_value() );
        w.send();
    }
    const auto reply = wait_reply( key, 60000 );
    if( !reply ) {
        add_msg( m_info, _( "The host did not answer.  Try again in a moment." ) );
        auto w = msg_writer( "talk_cancel" );
        w.send();
        return;
    }
    auto is = std::istringstream( *reply );
    auto jsin = JsonIn( is );
    auto o = jsin.get_object();
    o.allow_omitted_members();
    if( !o.get_bool( "ok" ) ) {
        add_msg( m_info, o.get_string( "why", "" ) );
        return;
    }
    auto missions = o.get_array( "missions" );
    while( missions.has_more() ) {
        mission::add_existing( mission_from_json( missions.next_string() ) );
    }
    // the dialogue runs with a private full copy of the NPC; the stand-in on the map
    // stays as it is and gets the result from the host afterwards
    auto copy = make_shared_fast<npc>();
    copy->setID( guy.getID() );
    copy->set_dimension( guy.get_dimension() );
    npc_from_json( *copy, o.get_string( "npc" ) );
    auto &real = *copy;

    const auto hidden = hide_missions_for( real, "host" );
    auto touched = missions_of( real );
    st.in_talk_body = true;
    try {
        real.talk_to_u( radio_contact, enforce_first_topic );
    } catch( ... ) {
        st.in_talk_body = false;
        throw;
    }
    st.in_talk_body = false;
    restore_missions( real, hidden );
    touched.merge( missions_of( real ) );

    auto w = msg_writer( "talk_done" );
    w.jo.member( "id", guy.getID().get_value() );
    w.jo.member( "missions" );
    w.jo.start_array();
    for( const auto uid : touched ) {
        if( const auto *m = mission::find( uid ) ) {
            w.jo.start_object();
            w.jo.member( "js", mission_to_json( *m ) );
            w.jo.member( "mine", coop_mission_access::player( *m ) == g->u.getID() );
            w.jo.end_object();
        }
    }
    w.jo.end_array();
    w.jo.member( "npc", npc_to_json( real ) );
    w.send();
}

// --- incoming messages -----------------------------------------------------

auto set_lockstep( bool on, const std::string &who ) -> void;
auto hud_banner( const std::string &text ) -> void;

/// Host: the free tile nearest to the host where the partner can appear. Only the host
/// knows where its NPCs and monsters stand.
auto partner_spawn_tile() -> tripoint_abs_ms
{
    const auto host_tile = g->u.bub_pos();
    auto dest = host_tile;
    auto best = 1000;
    for( const auto &cand : g->m.points_in_radius( host_tile, 4 ) ) {
        const auto dist = rl_dist( cand, host_tile );
        if( cand != host_tile && dist < best && g->m.passable( cand ) &&
            g->m.has_floor_or_support( cand ) && g->critter_at( cand ) == nullptr ) {
            dest = cand;
            best = dist;
        }
    }
    return g->u.abs_pos() + ( dest - host_tile );
}

auto send_welcome() -> void
{
    auto w = msg_writer( "welcome" );
    const auto *active = g->get_active_world();
    const WORLDINFO *world = active ? active->info : nullptr;
    w.jo.member( "ver", protocol_version );
    w.jo.member( "world", world ? world->world_name : std::string() );
    w.jo.member( "turn", my_turn() );
    write_pos( w.jo, "pos", g->u.abs_pos() );
    write_pos( w.jo, "spawn", partner_spawn_tile() );
    w.jo.member( "mods" );
    w.jo.start_array();
    if( world ) {
        for( const auto &mod : world->active_mod_order ) {
            w.jo.write( mod.str() );
        }
    }
    w.jo.end_array();
    w.jo.member( "opts" );
    w.jo.start_object();
    if( world ) {
        for( const auto &opt : world->WORLD_OPTIONS ) {
            w.jo.member( opt.first, opt.second.getValue( true ) );
        }
    }
    w.jo.end_object();
    w.send();
}

auto handle_message( const std::string &raw, const bool world_safe ) -> bool
{
    auto &st = s();
    auto is = std::istringstream( raw );
    auto jsin = JsonIn( is );
    auto o = jsin.get_object();
    o.allow_omitted_members();
    const auto type = o.get_string( "t" );
    if( type == "get" ) {
        host_handle_get( o );
        return true;
    }
    if( type == "talk_req" ) {
        host_handle_talk_req( o );
        return true;
    }
    if( type == "mnew" ) {
        host_handle_mission_new( o );
        return true;
    }
    // served at once even if the host is in a menu: the partner waits in a dialogue
    if( type == "massign" ) {
        host_handle_mission_assign( o );
        return true;
    }
    if( type == "talk_cancel" ) {
        st.talking_npc = character_id();
        return true;
    }
    if( type == "hello" ) {
        st.peer = peer_state{};
        st.peer.name = o.get_string( "name", "" );
        st.mon_sig.clear();
        send_welcome();
        return true;
    }
    if( !world_safe ) {
        return false;
    }
    st.screen_dirty = true;
    if( type == "st" ) {
        const auto first = !st.peer.in_game;
        st.peer.in_game = true;
        st.peer.turn = o.get_int( "turn" );
        st.peer.done = o.get_int( "done" );
        st.peer.danger = o.get_bool( "danger" );
        st.peer.active = o.get_bool( "active" );
        st.peer.name = o.get_string( "name" );
        st.peer.male = o.get_bool( "male" );
        if( is_host() && o.has_int( "me" ) ) {
            st.partner_avatar = character_id( o.get_int( "me" ) );
        }
        st.peer.pos = read_pos( o, "pos" );
        st.peer.has_pos = true;
        st.peer.activity = o.get_string( "act", "" );
        st.peer.in_veh = o.get_bool( "inveh", false );
        st.peer.ctrl = o.get_bool( "ctrl", false );
        st.peer.veh = o.get_string( "veh", "" );
        if( o.has_array( "mnt" ) ) {
            auto a = o.get_array( "mnt" );
            const auto x = a.next_int();
            const auto y = a.next_int();
            const auto z = a.next_int();
            st.peer.mount = tripoint_mnt_veh( x, y, z );
        }
        {
            auto look = read_look( o );
            st.peer.worn = std::move( look.worn );
            st.peer.wield = std::move( look.wield );
            st.peer.traits = std::move( look.traits );
            st.peer.light = look.light;
        }
        update_puppet();
        if( first ) {
            add_msg( m_good, _( "%s joined the game." ), st.peer.name );
            st.lockstep = false;
            if( is_host() ) {
                st.mon_sig.clear();
                host_send_monsters( true );
                st.npc_sig.clear();
                host_send_npcs( true );
                sync_vehicles( true );
                share_all_team_missions();
            }
            send_state( -1 );
        }
    } else if( type == "tiles" ) {
        auto d = o.get_array( "d" );
        st.applying_remote = true;
        while( d.has_more() ) {
            auto one = d.next_object();
            one.allow_omitted_members();
            apply_tile( one );
            refresh_sig( read_pos( one, "p" ) );
        }
        st.applying_remote = false;
    } else if( type == "vmove" ) {
        apply_vmove( o );
    } else if( type == "vfull" ) {
        apply_vfull( o );
    } else if( type == "snd" ) {
        apply_sounds( o );
    } else if( type == "vgone" ) {
        apply_vgone( o );
    } else if( type == "vreq" ) {
        host_handle_vreq( o );
    } else if( type == "drive" ) {
        host_handle_drive( o );
    } else if( type == "mon" ) {
        client_apply_monsters( o );
    } else if( type == "monc" ) {
        host_apply_monster_changes( o );
    } else if( type == "dmg" ) {
        const auto bp = bodypart_str_id( o.get_string( "bp" ) );
        const auto amount = o.get_int( "amt" );
        if( bp.is_valid() && amount > 0 ) {
            if( const auto src = o.get_string( "src", "" ); src.empty() ) {
                add_msg( m_bad, _( "You take %d damage." ), amount );
            } else {
                add_msg( m_bad, _( "%1$s hits you for %2$d damage." ), src, amount );
            }
            g->u.apply_damage( nullptr, bp.id(), amount );
        }
    } else if( type == "eff" ) {
        const auto eff = efftype_id( o.get_string( "id" ) );
        const auto bp = bodypart_str_id( o.get_string( "bp" ) );
        if( eff.is_valid() ) {
            g->u.add_effect( eff, time_duration::from_turns( o.get_int( "dur" ) ), bp,
                             o.get_int( "int" ) );
        }
    } else if( type == "npcs" ) {
        client_apply_npcs( o );
    } else if( type == "talk_done" ) {
        host_handle_talk_done( o );
    } else if( type == "mteam" ) {
        client_take_team_mission( o.get_string( "js" ) );
    } else if( type == "mfin" ) {
        const auto js = o.get_string( "js" );
        const auto incoming = mission_from_json( js );
        if( is_host() ) {
            mission_from_partner( js, true );
        } else {
            mission::add_existing( incoming );
        }
        if( auto *m = mission::find( incoming.get_id() ) ) {
            close_finished_mission( *m, st.peer.name );
        }
    } else if( type == "resync" ) {
        // the client lost some monsters or NPCs and wants everything again
        st.mon_sig.clear();
        st.npc_sig.clear();
        host_send_monsters( true );
        host_send_npcs( true );
        msg_writer( "resynced" ).send();
    } else if( type == "resynced" ) {
        st.resync_requested = false;
    } else if( type == "lock" ) {
        set_lockstep( o.get_bool( "on" ), st.peer.name );
    } else if( type == "ndmg" ) {
        // the client hurt one of the host's NPCs
        const auto bp = bodypart_str_id( o.get_string( "bp" ) );
        if( auto *guy = g->find_npc( character_id( o.get_int( "id" ) ) ); guy != nullptr && bp.is_valid() ) {
            guy->apply_damage( st.puppet.get(), bp.id(), o.get_int( "amt" ) );
        }
    } else if( type == "bye" ) {
        st.peer.in_game = false;
        remove_puppet();
        remove_ghosts();
        add_msg( m_warning, _( "Your partner left the game." ) );
    }
    return true;
}

std::vector<std::string> &deferred() // *NOPAD*
{
    static auto q = std::vector<std::string> {};
    return q;
}

/// Process queued messages. With @p world_safe false only requests that do not
/// change the world are served; the rest wait for a safe point.
auto pump( const bool world_safe ) -> void
{
    auto &st = s();
    if( !enabled() ) {
        return;
    }
    if( net::connection_generation() != st.net_generation ) {
        st.net_generation = net::connection_generation();
        st.peer = peer_state{};
        st.mon_sig.clear();
    }
    if( world_safe && !deferred().empty() ) {
        auto pending = std::vector<std::string> {};
        pending.swap( deferred() );
        for( const auto &raw : pending ) {
            try {
                handle_message( raw, true );
            } catch( const std::exception &err ) {
                DebugLog( DL::Error, DC::Main ) << "coop: bad message: " << err.what();
            }
        }
    }
    auto raw = std::string{};
    while( net::poll( raw ) ) {
        try {
            if( !handle_message( raw, world_safe ) ) {
                deferred().push_back( raw );
            }
        } catch( const std::exception &err ) {
            DebugLog( DL::Error, DC::Main ) << "coop: bad message: " << err.what();
        }
    }
    if( world_safe && st.peer.in_game && !net::connected() ) {
        st.talking_npc = character_id();
        st.peer.in_game = false;
        remove_puppet();
        remove_ghosts();
        add_msg( m_warning, _( "Connection to your partner was lost." ) );
    }
}

auto partner_present() -> bool
{
    return net::connected() && s().peer.in_game;
}

/// Client: every monster must come from the host. Remove the ones the client made itself
/// (old copies from its map files) and ask the host again for the ones it lost when
/// its map window moved.
auto client_cleanup() -> void
{
    auto &st = s();
    if( !is_client() || !partner_present() || st.transferring ) {
        return;
    }
    auto tracked = std::set<const monster *> {};
    auto lost = false;
    for( auto it = st.mon_by_id.begin(); it != st.mon_by_id.end(); ) {
        if( const auto ptr = it->second.lock(); ptr && !ptr->is_dead() ) {
            tracked.insert( ptr.get() );
            ++it;
        } else {
            st.mon_sig.erase( it->first );
            it = st.mon_by_id.erase( it );
            lost = true;
        }
    }
    auto stray = std::vector<monster *> {};
    for( monster &critter : g->all_monsters() ) {
        if( !tracked.contains( &critter ) ) {
            stray.push_back( &critter );
        }
    }
    for( monster *critter : stray ) {
        g->remove_zombie( *critter );
    }
    for( auto it = st.ghosts.begin(); it != st.ghosts.end(); ) {
        if( !is_active( it->second.get() ) ) {
            DebugLog( DL::Info, DC::Main ) << "coop: stand-in " << it->first << " was unloaded";
            remove_stand_in( it->second );
            it = st.ghosts.erase( it );
            lost = true;
        } else {
            ++it;
        }
    }
    if( lost && !st.resync_requested ) {
        msg_writer( "resync" ).send();
        st.resync_requested = true;
    }
}

auto lockstep_wait_text() -> std::string
{
    return string_format( _( "Turn-by-turn mode: waiting for your partner's move.\n"
                             "%s to switch to free movement." ), press_x( ACTION_COOP_LOCKSTEP ) );
}

auto set_lockstep( const bool on, const std::string &who ) -> void
{
    auto &st = s();
    if( st.lockstep == on ) {
        return;
    }
    st.lockstep = on;
    hud_banner( on ? string_format( _( "turn-by-turn ON (%s)" ), who ) :
                string_format( _( "free movement ON (%s)" ), who ) );
    if( on ) {
        add_msg( m_info, _( "%s turned on turn-by-turn mode: the world waits for both of you every turn." ),
                 who );
    } else {
        add_msg( m_info, _( "%s turned off turn-by-turn mode: you both move freely." ), who );
    }
}

// --- indicator ---------------------------------------------------------------

constexpr auto hud_width = 44;

auto hud_layout( ui_adaptor &ui ) -> void
{
    auto &st = s();
    // the map spans the screen between the side panels
    const auto left = panel_manager::get_manager().get_width_left();
    const auto right = panel_manager::get_manager().get_width_right();
    const auto map_width = std::max( 10, TERMX - left - right );
    const auto width = std::min( hud_width, map_width );
    const auto pos = point( left + ( map_width - width ) / 2, 0 );
    st.hud_win = catacurses::newwin( 1, width, pos );
    ui.position_from_window( st.hud_win );
}

auto hud_draw( ui_adaptor & ) -> void
{
    auto &st = s();
    werase( st.hud_win );
    auto color = c_light_green;
    auto text = std::string{};
    if( !partner_present() ) {
        color = c_dark_gray;
        text = is_host() ? _( "co-op: waiting for a partner" ) : _( "co-op: no connection" );
    } else if( st.waiting_now ) {
        color = c_light_red;
        text = st.lockstep ? _( "turn-by-turn: waiting for partner" ) :
               _( "partner is far behind, waiting" );
    } else if( st.lockstep ) {
        text = _( "turn-by-turn: your move" );
    } else {
        text = _( "free movement" );
    }
    if( !st.banner.empty() && clock_type::now() < st.banner_until ) {
        text = st.banner;
    }
    const auto line = colorize( "●", color ) + " " + text;
    const auto width = getmaxx( st.hud_win );
    const auto len = utf8_width( line, true );
    trim_and_print( st.hud_win, point( std::max( 0, ( width - len ) / 2 ), 0 ), width, c_white, line );
    wnoutrefresh( st.hud_win );
}

auto hud_refresh() -> void
{
    auto &st = s();
    if( st.hud ) {
        st.hud->invalidate_ui();
        ui_manager::redraw_invalidated();
        refresh_display();
    }
}

auto hud_banner( const std::string &text ) -> void
{
    auto &st = s();
    st.banner = text;
    st.banner_until = clock_type::now() + std::chrono::seconds( 4 );
    hud_refresh();
}

auto hud_create() -> void
{
    auto &st = s();
    st.hud = std::make_unique<ui_adaptor>();
    st.hud->on_screen_resize( hud_layout );
    st.hud->on_redraw( hud_draw );
    hud_layout( *st.hud );
}

/// Wait (pumping the network and keeping the screen alive) while @p keep_waiting is true.
/// The toggle key still works, so a player can leave turn-by-turn mode while waiting.
template<typename F>
auto wait_while( const std::string &text, const int timeout_ms, F keep_waiting ) -> void
{
    auto &st = s();
    const auto start = clock_type::now();
    auto ctxt = input_context( "DEFAULTMODE" );
    ctxt.register_action( "coop_lockstep" );
    ctxt.set_timeout( 30 );
    while( partner_present() && keep_waiting() ) {
        if( timeout_ms >= 0 && ms_since( start ) > timeout_ms ) {
            break;
        }
        if( !st.waiting_now && ms_since( start ) > 150 && !text.empty() ) {
            st.waiting_now = true;
            hud_refresh();
        }
        if( ctxt.handle_input() == "coop_lockstep" ) {
            toggle_lockstep();
        }
        if( !st.banner.empty() && clock_type::now() >= st.banner_until ) {
            st.banner.clear();
            hud_refresh();
        }
        pump( true );
    }
    if( st.waiting_now ) {
        st.waiting_now = false;
        hud_refresh();
    }
}

} // namespace

// --- public API ------------------------------------------------------------

auto role() -> role_t
{
    return s().role;
}

auto configure_host( const uint16_t port ) -> void
{
    s().role = role_t::host;
    s().port = port == 0 ? default_port : port;
}

auto configure_client( const std::string &address ) -> void
{
    auto &st = s();
    st.role = role_t::client;
    const auto colon = address.rfind( ':' );
    if( colon != std::string::npos ) {
        st.address = address.substr( 0, colon );
        st.port = static_cast<uint16_t>( std::atoi( address.substr( colon + 1 ).c_str() ) );
    } else {
        st.address = address;
        st.port = default_port;
    }
}

auto show_menu() -> void
{
    auto &st = s();
    const auto last_file = PATH_INFO::config_dir() + "coop_last_address.txt";
    auto menu = uilist{};
    menu.text = is_host() ? string_format( _( "Co-op: hosting on port %d." ), st.port ) :
                is_client() ? string_format( _( "Co-op: joining %s:%d." ), st.address, st.port ) :
                std::string( _( "Co-op is off." ) );
    menu.addentry( 0, true, 'h', _( "Host a game" ) );
    menu.addentry( 1, true, 'j', _( "Join a game" ) );
    menu.addentry( 2, enabled(), 'o', _( "Turn co-op off" ) );
    menu.query();
    if( menu.ret == 0 ) {
        auto ask = string_input_popup();
        const auto text = ask.title( _( "Port:" ) ).width( 8 ).text( std::to_string( default_port ) )
                          .query_string();
        if( ask.canceled() ) {
            return;
        }
        net::shutdown();
        configure_host( static_cast<uint16_t>( std::atoi( text.c_str() ) ) );
        popup( _( "Co-op host is on (port %d).\n\nNow start New Game or Load as usual.  "
                  "Your partner can join as soon as you are inside the world." ), st.port );
    } else if( menu.ret == 1 ) {
        auto last = std::string{};
        read_from_file( last_file, [&last]( std::istream & in ) {
            std::getline( in, last );
        }, true );
        auto ask = string_input_popup();
        const auto text = ask.title( _( "Host address (ip or ip:port):" ) ).width( 40 ).text( last )
                          .query_string();
        if( ask.canceled() || text.empty() ) {
            return;
        }
        write_to_file( last_file, [&text]( std::ostream & out ) {
            out << text << "\n";
        } );
        net::shutdown();
        configure_client( text );
        popup( _( "Co-op: will join %s.\n\nNew Game creates a new character next to the host.  "
                  "Load continues an earlier character in the local world made for this host.\n\n"
                  "The host must already be inside the world." ), text );
    } else if( menu.ret == 2 ) {
        net::shutdown();
        st.role = role_t::none;
    }
}

auto client_prepare_world() -> WORLDINFO * // *NOPAD*
{
    auto &st = s();
    auto err = std::string{};
    if( !net::connected() && !net::connect_to( st.address, st.port, err ) ) {
        popup( _( "Co-op: %s" ), err );
        return nullptr;
    }
    st.net_generation = net::connection_generation();
    {
        auto w = msg_writer( "hello" );
        w.jo.member( "ver", protocol_version );
        w.send();
    }
    auto raw = std::string{};
    const auto start = clock_type::now();
    auto got = false;
    while( ms_since( start ) < 60000 && net::connected() ) {
        if( net::wait_for_message( 100 ) && net::poll( raw ) && raw.find( "\"welcome\"" ) != std::string::npos ) {
            got = true;
            break;
        }
    }
    if( !got ) {
        popup( _( "Co-op: the host did not answer.  The host must be inside the game world." ) );
        net::shutdown();
        return nullptr;
    }
    auto is = std::istringstream( raw );
    auto jsin = JsonIn( is );
    auto o = jsin.get_object();
    o.allow_omitted_members();
    st.host_world = o.get_string( "world" );
    st.host_turn_at_join = o.get_int( "turn" );
    st.host_pos = read_pos( o, "pos" );
    st.spawn_pos = o.has_array( "spawn" ) ? read_pos( o, "spawn" ) : st.host_pos;

    auto mods = std::vector<mod_id> {};
    auto ma = o.get_array( "mods" );
    while( ma.has_more() ) {
        const auto id = mod_id( ma.next_string() );
        if( !id.is_valid() ) {
            popup( _( "Co-op: the host uses mod \"%s\" which is not installed here." ), id.str() );
            net::shutdown();
            return nullptr;
        }
        mods.push_back( id );
    }
    // the local mirror world is recognised by a marker file with the host world's name
    WORLDINFO *world = nullptr;
    for( const auto &name : world_generator->all_worldnames() ) {
        auto *cand = world_generator->get_world( name );
        auto marker = std::string{};
        read_from_file( cand->folder_path() + "/coop_host.txt", [&marker]( std::istream & in ) {
            std::getline( in, marker );
        }, true );
        if( marker == st.host_world ) {
            world = cand;
            break;
        }
    }
    if( world == nullptr ) {
        world = world_generator->make_new_world( mods );
        if( world == nullptr ) {
            popup( _( "Co-op: cannot create a local world." ) );
            return nullptr;
        }
        write_to_file( world->folder_path() + "/coop_host.txt", [&st]( std::ostream & out ) {
            out << st.host_world << "\n";
        } );
    }
    world->active_mod_order = mods;
    auto opts = o.get_object( "opts" );
    opts.allow_omitted_members();
    for( auto &opt : world->WORLD_OPTIONS ) {
        if( opts.has_string( opt.first ) ) {
            opt.second.setValue( opts.get_string( opt.first ) );
        }
    }
    world->save();
    {
        auto w = msg_writer( "get" );
        w.jo.member( "k", "artifacts" );
        w.send();
        if( const auto data = net::wait_blob( "artifacts", 30000 ); data && !data->empty() ) {
            write_to_file( world->folder_path() + "/artifacts.gsav", [&data]( std::ostream & out ) {
                out << *data;
            } );
        }
    }
    return world;
}

auto on_game_started() -> void
{
    auto &st = s();
    if( !enabled() ) {
        return;
    }
    st.in_game = true;
    st.sigs.clear();
    st.mon_ids.clear();
    st.mon_by_id.clear();
    st.mon_sig.clear();
    st.puppet.reset();
    st.last_done_sent = -1;
    st.lights.clear();
    hud_create();
    if( is_host() ) {
        auto err = std::string{};
        if( !net::host_listen( st.port, err ) ) {
            popup( _( "Co-op: %s" ), err );
        } else {
            add_msg( m_info, _( "Co-op: waiting for a partner on port %d." ), st.port );
        }
        return;
    }
    // client: jump next to the host
    calendar::turn = calendar::turn_zero + time_duration::from_turns( st.host_turn_at_join );
    // the jump may pass through a tile without a floor; nothing may hurt the character
    st.transferring = true;
    if( !g->m.inbounds( st.host_pos ) ) {
        g->place_player_overmap( project_to<coords::omt>( st.host_pos ) );
    }
    for( monster &critter : g->all_monsters() ) {
        g->remove_zombie( critter );
    }
    // the host's choice; if it does not fit here, the nearest free tile with a floor
    auto dest = abs_to_bub( st.spawn_pos );
    if( st.spawn_pos == st.host_pos || !g->m.passable( dest ) || g->critter_at( dest ) != nullptr ) {
        const auto host_tile = abs_to_bub( st.host_pos );
        dest = host_tile;
        auto best = 1000;
        for( const auto &cand : g->m.points_in_radius( host_tile, 3 ) ) {
            const auto dist = rl_dist( cand, host_tile );
            if( cand != host_tile && dist < best && g->m.passable( cand ) &&
                g->m.has_floor_or_support( cand ) && g->critter_at( cand ) == nullptr ) {
                dest = cand;
                best = dist;
            }
        }
    }
    g->place_player( dest );
    st.transferring = false;
    st.peer.pos = st.host_pos;
    st.peer.has_pos = true;
    st.peer.turn = st.host_turn_at_join;
    send_state( -1 );
}

auto on_game_ended() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return;
    }
    try {
        if( net::connected() ) {
            msg_writer( "bye" ).send();
        }
    } catch( const std::exception &err ) {
        DebugLog( DL::Error, DC::Main ) << "coop: leaving failed: " << err.what();
    }
    st.in_game = false;
    st.puppet.reset();
    st.peer = peer_state{};
    st.hud.reset();
    st.lights.clear();
    net::shutdown();
}

auto begin_turn() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return;
    }
    pump( true );
    if( !is_client() ) {
        return;
    }
    // In turn-by-turn mode the client starts the next turn only after the host;
    // otherwise it may run ahead of the host by a limited number of turns.
    const auto next = my_turn() + 1;
    const auto text = st.lockstep ? lockstep_wait_text() :
                      std::string( _( "Your partner is far behind in time (a menu is open?).  Waiting…" ) );
    wait_while( text, -1, [&]() {
        if( st.peer.turn >= next ) {
            return false;
        }
        return st.lockstep || next - st.peer.turn > max_client_lead;
    } );
}

auto before_world_sim() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return;
    }
    if( is_client() ) {
        client_send_monster_changes();
        scan_and_send_tiles();
    }
    send_state( my_turn() );
    pump( true );
    if( !is_host() || !partner_present() ) {
        return;
    }
    const auto turn = my_turn();
    const auto caught_up = [&]() {
        return st.peer.done >= turn || st.peer.turn > turn;
    };
    if( st.lockstep ) {
        // turn-by-turn: the world does not move until the partner has acted
        wait_while( lockstep_wait_text(), -1, [&]() {
            return st.lockstep && !caught_up();
        } );
    } else if( st.peer.active && ( g->u.activity || g->u.in_sleep_state() ) ) {
        // do not fast-forward a long action past a partner who is walking around
        wait_while( std::string(), 500, [&]() {
            return st.peer.active && !caught_up();
        } );
    }
}

auto end_turn() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return;
    }
    pump( true );
    update_puppet();
    client_cleanup();
    scan_and_send_tiles();
    if( is_host() ) {
        host_send_monsters( false );
        host_send_npcs( false );
    }
    sync_vehicles( false );
    flush_sounds();
    send_state( st.last_done_sent );
}

auto after_action() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return;
    }
    if( st.autopass ) {
        st.autopass = false;
    } else {
        st.last_input = clock_type::now();
    }
    if( !partner_present() ) {
        return;
    }
    if( is_client() ) {
        client_send_monster_changes();
    }
    scan_and_send_tiles();
    // a vehicle the player just used goes out before the partner's next update
    sync_vehicles( false );
    flush_sounds();
    send_state( st.last_done_sent );
}

auto idle_tick() -> bool
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return false;
    }
    pump( true );
    if( !st.banner.empty() && clock_type::now() >= st.banner_until ) {
        st.banner.clear();
        st.screen_dirty = true;
    }
    if( st.screen_dirty ) {
        st.screen_dirty = false;
        if( st.hud ) {
            st.hud->invalidate_ui();
        }
        update_puppet();
        client_cleanup();
        g->invalidate_main_ui_adaptor();
        ui_manager::redraw_invalidated();
    }
    if( !partner_present() ) {
        return false;
    }
    // a hint the first time an enemy shows up while both move freely
    const auto danger = compute_danger();
    if( danger && !st.danger_hint_shown && !st.lockstep ) {
        add_msg( m_info, _( "Co-op: %s to turn on turn-by-turn mode for a close fight." ),
                 press_x( ACTION_COOP_LOCKSTEP ) );
        st.danger_hint_shown = true;
    } else if( !danger ) {
        st.danger_hint_shown = false;
    }
    // in turn-by-turn mode nobody passes turns automatically
    if( st.lockstep || ms_since( st.last_input ) < active_window_ms ) {
        return false;
    }
    const auto turn = my_turn();
    auto pass = false;
    if( is_client() ) {
        pass = turn < st.peer.turn;
    } else {
        pass = st.peer.done >= turn || st.peer.turn > turn;
    }
    if( !pass ) {
        return false;
    }
    st.autopass = true;
    // Far behind (the partner did a long action): wait the gap out at full
    // speed instead of one turn per input poll. Not near enemies: the wait
    // would stop with a question every time one is seen.
    const auto gap = st.peer.turn - turn;
    if( gap > catch_up_min_turns && !danger && !( g->u.activity && *g->u.activity ) ) {
        auto act = std::make_unique<player_activity>( activity_id( "ACT_WAIT" ),
                   to_moves<int>( time_duration::from_turns( gap ) ), 0 );
        act->str_values.emplace_back( catch_up_tag );
        g->u.assign_activity( std::move( act ), false );
    }
    return true;
}

auto background_pump() -> void
{
    if( enabled() && s().in_game ) {
        pump( false );
    }
}

auto wants_pump() -> bool
{
    return enabled() && s().in_game;
}

auto client_read_map_omt( const std::string &dim, const tripoint_abs_omt &p,
                          const std::function<void( JsonIn & )> &reader ) -> bool
{
    const auto key = omt_key( dim, p );
    auto data = net::take_blob( key, 120000 );
    if( !data ) {
        auto w = msg_writer( "get" );
        w.jo.member( "k", "omt" );
        w.jo.member( "dim", dim );
        w.jo.member( "x", p.x() );
        w.jo.member( "y", p.y() );
        w.jo.member( "z", p.z() );
        w.send();
        data = net::wait_blob( key, 30000 );
    }
    if( !data || data->size() < 3 ) {
        return false;
    }
    auto is = std::istringstream( *data );
    auto jsin = JsonIn( is );
    reader( jsin );
    return true;
}

auto client_read_overmap( const std::string &dim, const point_abs_om &p,
                          const std::function<void( std::istream & )> &reader ) -> bool
{
    const auto key = om_key( dim, p );
    auto w = msg_writer( "get" );
    w.jo.member( "k", "om" );
    w.jo.member( "dim", dim );
    w.jo.member( "x", p.x() );
    w.jo.member( "y", p.y() );
    w.send();
    const auto data = net::wait_blob( key, 60000 );
    if( !data || data->empty() ) {
        return false;
    }
    auto is = std::istringstream( *data );
    reader( is );
    return true;
}

auto client_prefetch_window( const point_abs_sm &new_origin, const bool full ) -> void
{
    auto &st = s();
    if( !is_client() || !net::connected() ) {
        return;
    }
    const auto old_origin = g->m.get_abs_sub();
    const auto to_omt = []( const int sm ) {
        return sm >= 0 ? sm / 2 : -( ( -sm + 1 ) / 2 );
    };
    const auto id = string_format( "bulk|%d", ++st.bulk_counter );
    auto w = msg_writer( "get" );
    w.jo.member( "k", "bulk" );
    w.jo.member( "id", id );
    w.jo.member( "dim", g->get_current_dimension_id().str() );
    w.jo.member( "x0", to_omt( new_origin.x() ) );
    w.jo.member( "y0", to_omt( new_origin.y() ) );
    w.jo.member( "x1", to_omt( new_origin.x() + g_mapsize ) + 1 );
    w.jo.member( "y1", to_omt( new_origin.y() + g_mapsize ) + 1 );
    if( full ) {
        w.jo.member( "ox0", 0 );
        w.jo.member( "oy0", 0 );
        w.jo.member( "ox1", 0 );
        w.jo.member( "oy1", 0 );
    } else {
        w.jo.member( "ox0", to_omt( old_origin.x() ) + 1 );
        w.jo.member( "oy0", to_omt( old_origin.y() ) + 1 );
        w.jo.member( "ox1", to_omt( old_origin.x() + g_mapsize ) );
        w.jo.member( "oy1", to_omt( old_origin.y() + g_mapsize ) );
    }
    w.send();
    net::wait_blob( id, 120000 );
}

auto client_start_omt() -> tripoint_abs_omt
{
    return project_to<coords::omt>( s().host_pos );
}

auto before_save() -> void
{
    if( enabled() && s().in_game ) {
        try {
            remove_puppet();
        } catch( const std::exception &err ) {
            DebugLog( DL::Error, DC::Main ) << "coop: puppet removal failed: " << err.what();
        }
    }
}

auto intercept_talk( npc &guy, const bool radio_contact, const bool enforce_first_topic ) -> bool
{
    auto &st = s();
    if( !enabled() || !st.in_game || st.in_talk_body ) {
        return false;
    }
    if( st.puppet && &guy == st.puppet.get() ) {
        add_msg( m_info, _( "Use your own voice to talk to your partner." ) );
        return true;
    }
    if( runs_world() ) {
        if( st.talking_npc.is_valid() && st.talking_npc == guy.getID() ) {
            add_msg( m_info, _( "%1$s is talking to %2$s." ), guy.disp_name(), st.peer.name );
            return true;
        }
        st.host_talking = guy.getID();
        const auto hidden = hide_missions_for( guy, "partner" );
        st.in_talk_body = true;
        try {
            guy.talk_to_u( radio_contact, enforce_first_topic );
        } catch( ... ) {
            st.in_talk_body = false;
            st.host_talking = character_id();
            throw;
        }
        st.in_talk_body = false;
        st.host_talking = character_id();
        restore_missions( guy, hidden );
        return true;
    }
    if( ghost_id_of( guy ) < 0 ) {
        return false;
    }
    if( !partner_present() ) {
        add_msg( m_info, _( "No connection to the host." ) );
        return true;
    }
    client_talk( guy, radio_contact, enforce_first_topic );
    return true;
}

auto is_frozen( const npc &guy ) -> bool
{
    const auto &st = s();
    return enabled() && st.talking_npc.is_valid() && st.talking_npc == guy.getID();
}

auto client_reserve_mission( const mission_type_id &type, const character_id &npc_id ) -> mission * // *NOPAD*
{
    auto &st = s();
    if( !is_client() || !partner_present() ) {
        return nullptr;
    }
    const auto key = string_format( "mnew:%d", ++st.req_counter );
    {
        auto w = msg_writer( "mnew" );
        w.jo.member( "req", key );
        w.jo.member( "type", type.str() );
        w.jo.member( "npc", npc_id.get_value() );
        w.send();
    }
    const auto reply = wait_reply( key, 60000 );
    if( !reply || reply->empty() ) {
        return nullptr;
    }
    const auto m = mission_from_json( *reply );
    mission::add_existing( m );
    return mission::find( m.get_id() );
}

auto client_assign_mission( mission &miss, avatar &u ) -> bool
{
    auto &st = s();
    if( !is_client() || !partner_present() ) {
        return false;
    }
    const auto key = string_format( "massign:%d", ++st.req_counter );
    {
        auto w = msg_writer( "massign" );
        w.jo.member( "req", key );
        w.jo.member( "uid", miss.get_id() );
        w.jo.member( "me", u.getID().get_value() );
        w.send();
    }
    const auto reply = wait_reply( key, 120000 );
    if( !reply ) {
        add_msg( m_bad, _( "The host did not answer; the mission was not taken." ) );
        return true;
    }
    auto is = std::istringstream( *reply );
    auto jsin = JsonIn( is );
    auto o = jsin.get_object();
    o.allow_omitted_members();
    if( !o.get_bool( "ok" ) ) {
        add_msg( m_bad, _( "Somebody has already taken this mission." ) );
        return true;
    }
    // the mission object stays where it is; only its content changes
    mission::add_existing( mission_from_json( o.get_string( "mission" ) ) );
    coop_mission_access::finish_on( miss, u );
    if( o.has_string( "copy" ) ) {
        const auto copy = mission_from_json( o.get_string( "copy" ) );
        mission::add_existing( copy );
        st.pending_copies.emplace_back( copy.get_id(), "host" );
    }
    return true;
}

auto on_mission_assigned( mission &miss ) -> void
{
    auto &st = s();
    if( !enabled() || !runs_world() ) {
        return;
    }
    record_kind( miss );
    if( is_team( miss ) ) {
        share_team_mission( miss );
        return;
    }
    if( !is_personal( miss ) ) {
        return;
    }
    const auto *giver = g->find_npc( miss.get_npc_id() );
    if( giver == nullptr || !giver->get_value( excl_key( miss.get_id() ) ).empty() ) {
        // no giver, or this already is the copy for one of the players
        return;
    }
    if( auto *copy = mission::reserve_new( miss.get_type().id, miss.get_npc_id() ) ) {
        st.pending_copies.emplace_back( copy->get_id(), "partner" );
        if( !st.in_talk_body ) {
            // not in a dialogue (a scripted assignment): add it right away
            if( auto *owner = g->find_npc( miss.get_npc_id() ) ) {
                restore_missions( *owner, {} );
            }
        }
    }
}

auto on_mission_finished( mission &miss ) -> void
{
    if( !enabled() || !s().in_game || !net::connected() ) {
        return;
    }
    if( runs_world() ) {
        if( coop_mission_access::player( miss ) == g->u.getID() && is_team( miss ) ) {
            auto w = msg_writer( "mfin" );
            w.jo.member( "js", mission_for_partner( miss ) );
            w.send();
        }
        return;
    }
    if( coop_mission_access::player( miss ) == g->u.getID() && !s().in_talk_body ) {
        // outside a dialogue (a deadline); in a dialogue the result goes with the NPC
        auto w = msg_writer( "mfin" );
        w.jo.member( "js", mission_to_json( miss ) );
        w.send();
    }
}

auto is_other_players_mission( const mission &miss ) -> bool
{
    if( !enabled() ) {
        return false;
    }
    const auto who = coop_mission_access::player( miss );
    if( runs_world() ) {
        return who == partner_id;
    }
    return who.is_valid() && who != g->u.getID();
}

auto stand_in_light( const Character &who ) -> std::optional<float>
{
    if( !enabled() ) {
        return std::nullopt;
    }
    const auto &lights = s().lights;
    if( const auto it = lights.find( &who ); it != lights.end() ) {
        return it->second;
    }
    return std::nullopt;
}

auto is_puppet( const Creature &c ) -> bool
{
    const auto &st = s();
    if( st.puppet && static_cast<const Creature *>( st.puppet.get() ) == &c ) {
        return true;
    }
    return st.building || ( !st.ghosts.empty() && ghost_id_of( c ) >= 0 );
}

auto intercept_damage( Character &who, Creature *source, const bodypart_id &bp,
                       const int amount ) -> bool
{
    if( enabled() && s().transferring ) {
        return true;
    }
    if( !enabled() || !is_puppet( who ) ) {
        return false;
    }
    if( s().building ) {
        return true;
    }
    if( const auto ghost = ghost_id_of( who ); ghost >= 0 ) {
        if( amount > 0 && net::connected() ) {
            auto w = msg_writer( "ndmg" );
            w.jo.member( "id", ghost );
            w.jo.member( "bp", bp.id().str() );
            w.jo.member( "amt", amount );
            w.send();
        }
        return true;
    }
    if( amount > 0 && net::connected() ) {
        auto w = msg_writer( "dmg" );
        w.jo.member( "bp", bp.id().str() );
        w.jo.member( "amt", amount );
        auto src = std::string{};
        // crashes hurt passengers with the driver as the source
        if( source != nullptr && source != &who && !( source->is_avatar() && who.in_vehicle ) ) {
            src = source->is_avatar() ? g->u.name : source->disp_name();
        }
        w.jo.member( "src", src );
        w.send();
    }
    return true;
}

auto intercept_effect( Creature &who, const efftype_id &eff, const time_duration &dur,
                       const bodypart_str_id &bp, const int intensity ) -> bool
{
    if( enabled() && s().transferring ) {
        return true;
    }
    if( !enabled() || !is_puppet( who ) ) {
        return false;
    }
    if( s().building ) {
        return true;
    }
    if( ghost_id_of( who ) >= 0 ) {
        // during a dialogue the copy is the real NPC; the host gets its state afterwards
        return !s().in_talk_body;
    }
    if( net::connected() ) {
        auto w = msg_writer( "eff" );
        w.jo.member( "id", eff.str() );
        w.jo.member( "dur", to_turns<int>( dur ) );
        w.jo.member( "bp", bp.str() );
        w.jo.member( "int", intensity );
        w.send();
    }
    return true;
}

auto on_sound( const sound_event &se ) -> void
{
    auto &st = s();
    if( !enabled() || st.playing_partner_sound || !partner_present() ) {
        return;
    }
    // weather runs on both machines
    if( se.category == sounds::sound_t::weather || !g->m.inbounds( se.origin ) ) {
        return;
    }
    st.sounds.push_back( { .pos = bub_to_abs( se.origin ), .se = se } );
}

auto partner_drives( const vehicle &veh ) -> bool
{
    auto &st = s();
    if( !enabled() || !is_host() || !st.puppet || !st.puppet->controlling_vehicle ) {
        return false;
    }
    const auto vp = g->m.veh_at( st.puppet->bub_pos() );
    return vp && &vp->vehicle() == &veh;
}

auto forward_drive( vehicle &veh, const tripoint_rel_veh &p ) -> void
{
    if( !enabled() || !is_client() || !partner_present() ) {
        return;
    }
    const auto id = veh_id_of( veh );
    if( id.empty() ) {
        return;
    }
    auto w = msg_writer( "drive" );
    w.jo.member( "id", id );
    w.jo.member( "x", p.x() );
    w.jo.member( "y", p.y() );
    w.jo.member( "z", p.z() );
    w.send();
}

auto on_main_ui_created() -> void
{
    auto &st = s();
    if( enabled() && st.in_game ) {
        st.hud.reset();
        hud_create();
    }
}

auto toggle_lockstep() -> void
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        add_msg( m_info, _( "Turn-by-turn mode is for co-op games." ) );
        return;
    }
    set_lockstep( !st.lockstep, g->u.name );
    if( net::connected() ) {
        auto w = msg_writer( "lock" );
        w.jo.member( "on", st.lockstep );
        w.send();
    }
}

auto status_text() -> std::string
{
    const auto &st = s();
    if( !enabled() ) {
        return std::string();
    }
    if( !partner_present() ) {
        return is_host() ? _( "Co-op: no partner" ) : _( "Co-op: disconnected" );
    }
    return string_format( _( "Co-op: %1$s%2$s" ), st.peer.name,
                          st.peer.danger ? _( " (in danger)" ) : "" );
}

} // namespace coop
