# Co-op: notes for development

This file is for the people (and the assistant) who work on co-op. Players read `COOP.md`.
A new chat about co-op starts by reading this file, `COOP.md` and `AGENTS.md`.

## Goal

Two players in one Cataclysm: BN world, modelled on CDDACoop for DDA. The client must feel
like a full game of its own: what one player sees must match what the other sees. Any
visible difference between the two machines (a monster that looks different, a corpse that
is only on one side) is a bug.

## Architecture

- **Host is authoritative.** The host runs the world: monsters, NPCs, vehicle physics, fields,
  missions. The client is a full game copy that does not simulate the world; it simulates only
  its own character.
- **Map data.** The client reads map chunks and overmaps from the host (`world::read_map_omt`,
  `world::read_overmap` hooks, bulk prefetch). On the client an evicted map area is dropped,
  not saved (`mapbuffer::unload_omt`), so it is read from the host again.
- **Transport** (`src/coop/coop_net.*`): TCP, length-prefixed frames, zlib, a keyed "blob"
  channel. A reader thread fills an inbox; `pump()` handles messages on the main thread.
- **World-safe points.** Messages that change the world are deferred until a turn boundary
  (`begin_turn`, `before_world_sim`, `end_turn`, `idle_tick`). While a menu is open,
  `apply_menu_safe()` applies only updates that create or delete nothing a menu may point at:
  partner state (`st`), vehicle motion (`vmove`, numbered by `seq`), sounds (`snd`).
- **Sync by signatures.** Tiles (terrain, furniture, traps, fields, items) are compared by
  hash around both players (`scan_radius` submaps, z ±1) and sent both ways. Tiles around a
  kill are sent wherever they are (`mark_death_area`).
- **Monsters**: replicated from the host with co-op ids (`mon` / `monc`); the client reports
  its hits and kills. Sprite variants use the co-op id as seed (`monster_look_seed`).
- **NPCs**: stand-ins on the client with host ids. Dialogue runs on the client on a private
  copy (`talk_req` / `talk_done`); the host freezes the NPC meanwhile. Followers walk after
  the player who recruited them (`coop_leader` value, `leader_of()` in NPC AI).
- **NPC orders ("C" menu, `game::chat`)**: the host sends each ally's attitude, mission
  and follower rules with the NPC update (`att`, `mis`, `rules`), so the client's menu
  shows the right entries. Client orders go to the host as `nord` (`forward_npc_order`:
  guard, follow, move_to, wake_up, mount, dismount) and changed rules as `nord` with
  `rules` (`send_follower_rules`); the host runs them on the real NPC. "Control NPC" is
  off on the client. Stand-ins ignore sounds (`npc::handle_sound`): the host's NPC answers
  an order and its words reach the client as a sound. `is_partner()` = only the partner's
  stand-in, `is_puppet()` = any stand-in.
- **Partner stand-in ("puppet")**: an NPC on each machine for the other player; it carries
  appearance, light, seat in a vehicle.
- **Vehicles**: co-op id in a tag `coop_veh_<id>`; host sends motion and full vehicles, the
  client sends full vehicles after its changes; driving commands go to the host
  (`forward_drive`); `partner_drives()` feeds the host's physics.
- **Time**: free mode (an idle player auto-passes, fast catch-up as a wait activity) and
  turn-by-turn mode (F5). The client may lead the host by up to 120 turns.
- **Seed**: the client uses the host's world seed, so weather is the same.
  Thunder and lightning flashes use `coop::weather_one_in` instead of `one_in`: the roll is
  a hash of the world seed, the turn and the event kind, so both machines get the same events
  on the same turn with no message (checked: 1 h of lightning storm, identical turns).
- **Overmap knowledge** (`omk`): `overmapbuffer::set_seen` (only on unseen -> seen) and
  `toggle_explored` call `coop::on_overmap_seen` / `on_overmap_explored`; the tiles are
  sent at `end_turn` / `after_action`. The receiver writes the flags straight into the
  overmap (no echo, no `spawn_ores`). On join each side sends all seen/explored tiles of the
  3x3 overmaps around its player (`share_overmap_knowledge`). Not menu-safe: waits while
  the overmap screen is open.

## Code rules

- Follow `AGENTS.md` (trailing return types, `auto`, designated initializers, `std::optional`,
  anonymous namespaces, triple-slash doc comments).
- Follow the BN developer docs: <https://docs.cataclysmbn.org/dev/> (also in `docs/en/dev/`):
  best practices (free functions over members, no getter/setter pairs, minimal includes in
  headers, forward declarations), code style, game objects (`safe_reference` for objects kept
  across turns), coordinates (`tripoint_abs_ms` / `tripoint_bub_ms`).
- Do not change widely used headers (`game.h`, `map.h`, `vehicle.h`, `character.h`) unless
  needed: a friend struct (`coop_mission_access`, `coop_vehicle_access`) is the accepted
  way to reach private fields.
- Hooks in game code stay one or two lines and call into `coop::`; logic lives in `src/coop/`.

## Testing

- Local (assistant's Linux workspace, fresh in every chat, full build ~1.5 h on 2 cores;
  start it first and read code meanwhile):
  - SDL3 is needed even for curses; the CMake download from GitHub archives is blocked,
    so clone it: `git clone --depth 1 -b release-3.4.8 https://github.com/libsdl-org/SDL.git
    /home/claude/deps/sdl3`.
  - GCC 13 lacks `std::ranges::to`; use clang. `apt-get install g++-14` (for libstdc++ 14).
    clang 18 does not see `std::expected` in libstdc++ 14 (`__cpp_concepts` too old): either
    use clang 19 (`apt-get install clang-19`, `-DCMAKE_CXX_COMPILER=clang++-19`) or patch
    line 38 of `/usr/include/c++/14/expected` to `#if __cplusplus > 202002L`.
  - `cmake -S . -B out/c -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    -DCMAKE_BUILD_TYPE=Release -DCURSES=ON -DTILES=OFF -DSOUND=OFF -DTESTS=OFF -DLOCALIZE=OFF
    -DLANGUAGES=none -DJSON_FORMAT=OFF -DCATA_FORMAT_TARGETS=OFF
    -DFETCHCONTENT_SOURCE_DIR_SDL3=/home/claude/deps/sdl3 -DSDL_X11=OFF -DSDL_WAYLAND=OFF
    -DSDL_UNIX_CONSOLE_BUILD=ON` then `ninja -C out/c -j2 cataclysm-bn`.
  - Run from the repo root with `LANG=C.UTF-8 TERM=xterm-256color ./out/c/src/cataclysm-bn
    --datadir <repo>/data/ --userdir /tmp/h/ --coop-host` (and `/tmp/c/`, `--coop-join
    127.0.0.1`) in tmux sessions `h` / `c`, 140x45. "Play Now!" works for both.
  - Lua checks: `overmapbuffer.seen(TripointAbsOmt.new(x,y,z))`, `overmapbuffer.reveal(...)`,
    `gapi.place_player_overmap_at(...)`.
- tmux tips: run `stty -ixon` in the panes (Ctrl+S
  is XOFF otherwise). Type into text fields one character at a time (the game drops repeated
  keys). Debug menu: F12. Lua console: F12 → l, Enter to edit, Ctrl+S to run.
- Publishing: a direct push to `main` is refused in these sessions. Add the repo with push
  access (`add_repo`), push a branch, open a PR with `gh api repos/tsvetkovdm/CBN-Coop/pulls`
  (GraphQL `gh pr create` is unavailable); the user merges it.
- Windows builds: GitHub Actions in `tsvetkovdm/CBN-Coop`, workflow `matrix`, artifact
  `windows-tiles-x64-msvc-*`. Tiles-only code (`cata_tiles.cpp`) is compiled only there.
- Local Windows build (user's PC, verified with VS 2026 + vcpkg from the VS installer):
  `coop-build.bat` / `coop-build.ps1` in the repo root, same
  cmake flags as the CI Windows job, installs a playable folder to `out\game`. Setup and the
  two-instance launch (`--userdir host\` / `--userdir client\`): `COOP_BUILD_WINDOWS.md`.
  Keep its configure flags in sync with `.github/workflows/build.yml` when CI changes.

## Backlog

Visible differences between host and client (fix first):

- Tile sync covers z ±1 around players; a player high in a helicopter does not see ground
  changes except around kills.
- Fields (fire, smoke) far from both players are not updated.

- Ore notes underground (`overmap::spawn_ores`) are rolled with `rng` on the machine that
  sees the rock first; host and client may get different ores. Overmap notes are not shared.

Features:

- Smooth movement animation of the partner.
- Shared overmap notes (`N` on the overmap) and live partner reveals while the overmap
  screen is open.
- Host aiming pauses the world for both (known limitation; would need a rework of the
  aiming screen to keep its target list valid while the world runs).

Code health:

- `src/coop/coop.cpp` is one large file; split it by topic (map sync, monsters, NPCs and
  dialogue, missions, vehicles, sounds, time, HUD).
