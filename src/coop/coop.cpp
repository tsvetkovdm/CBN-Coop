#include "coop/coop.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "artifact.h"
#include "avatar.h"
#include "calendar.h"
#include "character_functions.h"
#include "coop/coop_net.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "debug.h"
#include "fstream_utils.h"
#include "game.h"
#include "item.h"
#include "json.h"
#include "map/field.h"
#include "map/map.h"
#include "map/mapbuffer.h"
#include "map/mapbuffer_registry.h"
#include "map/submap.h"
#include "messages.h"
#include "mod_manager.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap/overmap.h"
#include "overmap/overmapbuffer.h"
#include "path_info.h"
#include "player_activity.h"
#include "popup.h"
#include "string_input_popup.h"
#include "ui.h"
#include "trap.h"
#include "ui_manager.h"
#include "world.h"
#include "worldfactory.h"

namespace coop
{

namespace
{

using clock_type = std::chrono::steady_clock;

constexpr auto default_port = uint16_t{ 4580 };
constexpr auto protocol_version = 1;
/// How many turns the client may run ahead of the host while nobody is in danger.
constexpr auto max_client_lead = 120;
/// A player who pressed a key less than this long ago is "active" and is not auto-passed.
constexpr auto active_window_ms = 1500;
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

auto remove_puppet() -> void
{
    auto &st = s();
    if( !st.puppet ) {
        return;
    }
    const auto id = st.puppet->getID();
    get_overmapbuffer( g->get_current_dimension_id() ).remove_npc( id );
    const auto &npcs = g->raw_npcs();
    if( std::ranges::any_of( npcs, [id]( const auto & n ) { return n->getID() == id; } ) ) {
        g->erase_npc( id );
    }
    st.puppet.reset();
}

auto ensure_puppet() -> void
{
    auto &st = s();
    if( st.puppet || !st.peer.has_pos || !g->m.inbounds( st.peer.pos ) ) {
        return;
    }
    auto guy = make_shared_fast<npc>();
    character_funcs::normalize( *guy );
    guy->name = st.peer.name.empty() ? std::string( "Partner" ) : st.peer.name;
    guy->male = st.peer.male;
    guy->set_fac( faction_id( "your_followers" ) );
    guy->set_attitude( NPCATT_FOLLOW );
    guy->set_mission( NPC_MISSION_NULL );
    const auto proj = project_remain<coords::sm>( st.peer.pos );
    guy->spawn_at_precise( proj.quotient, proj.remainder_tripoint );
    get_overmapbuffer( g->get_current_dimension_id() ).insert_npc( guy );
    st.puppet = guy;
    st.puppet_set_pos = st.peer.pos;
    g->load_npcs();
}

auto update_puppet() -> void
{
    auto &st = s();
    if( !st.peer.in_game || !st.peer.has_pos ) {
        return;
    }
    ensure_puppet();
    if( !st.puppet ) {
        return;
    }
    if( !g->m.inbounds( st.peer.pos ) ) {
        return;
    }
    if( st.puppet->abs_pos() != st.peer.pos ) {
        st.puppet->setpos( st.peer.pos );
        st.puppet_set_pos = st.peer.pos;
    }
    st.puppet->moves = 0;
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
    write_pos( w.jo, "pos", g->u.abs_pos() );
    w.jo.member( "act", g->u.activity && *g->u.activity ? g->u.activity->id().str() : std::string() );
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
        if( existing && existing->is_dead() ) {
            existing = nullptr;
        }
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
            if( const auto ptr = it->second.lock() ) {
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
        apply_monster_json( *ptr, read_pos( one, "pos" ), one.get_string( "js" ) );
        st.mon_sig[id] = monster_sig( *ptr );
    }
    auto dead = o.get_array( "dead" );
    while( dead.has_more() ) {
        const auto id = dead.next_int();
        const auto it = st.mon_by_id.find( id );
        if( it == st.mon_by_id.end() ) {
            continue;
        }
        if( const auto ptr = it->second.lock() ) {
            // the client already dropped the corpse and loot on its side
            g->remove_zombie( *ptr );
        }
        st.mon_by_id.erase( it );
        st.mon_sig.erase( id );
    }
}

// --- incoming messages -----------------------------------------------------

auto send_welcome() -> void
{
    auto w = msg_writer( "welcome" );
    const auto *active = g->get_active_world();
    const WORLDINFO *world = active ? active->info : nullptr;
    w.jo.member( "ver", protocol_version );
    w.jo.member( "world", world ? world->world_name : std::string() );
    w.jo.member( "turn", my_turn() );
    write_pos( w.jo, "pos", g->u.abs_pos() );
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
        st.peer.pos = read_pos( o, "pos" );
        st.peer.has_pos = true;
        st.peer.activity = o.get_string( "act", "" );
        update_puppet();
        if( first ) {
            add_msg( m_good, _( "%s joined the game." ), st.peer.name );
            if( is_host() ) {
                st.mon_sig.clear();
                host_send_monsters( true );
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
    } else if( type == "mon" ) {
        client_apply_monsters( o );
    } else if( type == "monc" ) {
        host_apply_monster_changes( o );
    } else if( type == "dmg" ) {
        const auto bp = bodypart_str_id( o.get_string( "bp" ) );
        const auto amount = o.get_int( "amt" );
        if( bp.is_valid() && amount > 0 ) {
            add_msg( m_bad, _( "%1$s hits you for %2$d damage." ), o.get_string( "src", _( "Something" ) ),
                     amount );
            g->u.apply_damage( nullptr, bp.id(), amount );
        }
    } else if( type == "eff" ) {
        const auto eff = efftype_id( o.get_string( "id" ) );
        const auto bp = bodypart_str_id( o.get_string( "bp" ) );
        if( eff.is_valid() ) {
            g->u.add_effect( eff, time_duration::from_turns( o.get_int( "dur" ) ), bp,
                             o.get_int( "int" ) );
        }
    } else if( type == "bye" ) {
        st.peer.in_game = false;
        remove_puppet();
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
        st.peer.in_game = false;
        remove_puppet();
        add_msg( m_warning, _( "Connection to your partner was lost." ) );
    }
}

auto partner_present() -> bool
{
    return net::connected() && s().peer.in_game;
}

/// Wait (pumping the network and keeping the screen alive) while @p keep_waiting is true.
template<typename F>
auto wait_while( const std::string &text, const int timeout_ms, F keep_waiting ) -> void
{
    const auto start = clock_type::now();
    auto shown = false;
    auto popup = std::unique_ptr<static_popup> {};
    while( partner_present() && keep_waiting() ) {
        if( timeout_ms >= 0 && ms_since( start ) > timeout_ms ) {
            break;
        }
        if( !shown && ms_since( start ) > 400 && !text.empty() ) {
            popup = std::make_unique<static_popup>();
            popup->message( "%s", text );
            ui_manager::redraw();
            refresh_display();
            shown = true;
        }
        net::wait_for_message( 20 );
        pump( true );
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
    g->place_player_overmap( project_to<coords::omt>( st.host_pos ) );
    for( monster &critter : g->all_monsters() ) {
        g->remove_zombie( critter );
    }
    auto dest = abs_to_bub( st.host_pos );
    for( const auto &cand : g->m.points_in_radius( dest, 2 ) ) {
        if( cand != dest && g->m.passable( cand ) && g->critter_at( cand ) == nullptr ) {
            dest = cand;
            break;
        }
    }
    g->place_player( dest );
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
    if( net::connected() ) {
        msg_writer( "bye" ).send();
    }
    st.in_game = false;
    st.puppet.reset();
    st.peer = peer_state{};
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
    // The client may start the next turn when the host has reached it, or, while
    // nobody is in danger, when it is not too far ahead of the host.
    const auto next = my_turn() + 1;
    wait_while( _( "Waiting for your partner…" ), -1, [&]() {
        if( st.peer.turn >= next ) {
            return false;
        }
        const auto relaxed = !st.my_danger && !st.peer.danger;
        return !( relaxed && next - st.peer.turn <= max_client_lead );
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
    if( st.peer.danger ) {
        // strict turns: the world does not move until the partner has acted
        wait_while( _( "Waiting for your partner…" ), -1, [&]() {
            return st.peer.danger && !caught_up();
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
    scan_and_send_tiles();
    if( is_host() ) {
        host_send_monsters( false );
    }
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
    send_state( st.last_done_sent );
}

auto idle_tick() -> bool
{
    auto &st = s();
    if( !enabled() || !st.in_game ) {
        return false;
    }
    pump( true );
    if( st.screen_dirty ) {
        st.screen_dirty = false;
        update_puppet();
        g->invalidate_main_ui_adaptor();
        ui_manager::redraw_invalidated();
    }
    if( !partner_present() ) {
        return false;
    }
    if( ms_since( st.last_input ) < active_window_ms ) {
        return false;
    }
    const auto turn = my_turn();
    auto pass = false;
    if( is_client() ) {
        pass = turn < st.peer.turn;
    } else {
        st.my_danger = compute_danger();
        pass = !st.my_danger && ( st.peer.done >= turn || st.peer.turn > turn );
    }
    if( pass ) {
        st.autopass = true;
    }
    return pass;
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
        remove_puppet();
    }
}

auto is_puppet( const Creature &c ) -> bool
{
    const auto &st = s();
    return st.puppet && static_cast<const Creature *>( st.puppet.get() ) == &c;
}

auto intercept_damage( Character &who, Creature *source, const bodypart_id &bp,
                       const int amount ) -> bool
{
    if( !enabled() || !is_puppet( who ) ) {
        return false;
    }
    if( amount > 0 && net::connected() ) {
        auto w = msg_writer( "dmg" );
        w.jo.member( "bp", bp.id().str() );
        w.jo.member( "amt", amount );
        w.jo.member( "src", source ? source->disp_name() : std::string() );
        w.send();
    }
    return true;
}

auto intercept_effect( Creature &who, const efftype_id &eff, const time_duration &dur,
                       const bodypart_str_id &bp, const int intensity ) -> bool
{
    if( !enabled() || !is_puppet( who ) ) {
        return false;
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
