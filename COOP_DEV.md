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
- **Partner stand-in ("puppet")**: an NPC on each machine for the other player; it carries
  appearance, light, seat in a vehicle.
- **Vehicles**: co-op id in a tag `coop_veh_<id>`; host sends motion and full vehicles, the
  client sends full vehicles after its changes; driving commands go to the host
  (`forward_drive`); `partner_drives()` feeds the host's physics.
- **Time**: free mode (an idle player auto-passes, fast catch-up as a wait activity) and
  turn-by-turn mode (F5). The client may lead the host by up to 120 turns.
- **Seed**: the client uses the host's world seed, so weather is the same.

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

- Local: curses build in `/home/claude/src/bn/out/c`, two instances in tmux (`h` host,
  `c` client, `--coop-host` / `--coop-join 127.0.0.1`). Run `stty -ixon` in the panes (Ctrl+S
  is XOFF otherwise). Type into text fields one character at a time (the game drops repeated
  keys). Debug menu: F12. Lua console: F12 → l, Enter to edit, Ctrl+S to run.
- Windows builds: GitHub Actions in `tsvetkovdm/CBN-Coop`, workflow `matrix`, artifact
  `windows-tiles-x64-msvc-*`. Tiles-only code (`cata_tiles.cpp`) is compiled only there.

## Backlog

Visible differences between host and client (fix first):

- Overmap: what one player explores is not revealed to the other.
- Lightning and other weather events are rolled on each machine.
- Tile sync covers z ±1 around players; a player high in a helicopter does not see ground
  changes except around kills.
- NPC orders from the "C" menu (guard, go to, wake up) on the client act on stand-ins only.
- Fields (fire, smoke) far from both players are not updated.

Features:

- Smooth movement animation of the partner.
- Host aiming pauses the world for both (known limitation; would need a rework of the
  aiming screen to keep its target list valid while the world runs).

Code health:

- `src/coop/coop.cpp` is one large file; split it by topic (map sync, monsters, NPCs and
  dialogue, missions, vehicles, sounds, time, HUD).
