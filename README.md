# as_admin_time (Plugify)

Records how long every admin is online in the shared admin database (table `as_admin_time`, kept byte-compatible with the Metamod module so both server flavours can share the rows): connect/disconnect timestamps, played time, per-server id, plus crash recovery ('reset') and optional spectator-time exclusion ('spec_time'). Ported from the Pisex admin_system module 'AdminTime' ([AS] Admin Time by Pisex). Uses the database settings exported by the admin_system core. Requires the admin_system core.

Plugify port (fork) of **AdminTime ([AS] Admin Time)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_admin_time-1.0.3.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_admin_time`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

