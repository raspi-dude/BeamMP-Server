# Uninstalling what was added

Two features were added on top of upstream BeamMP-Server:

1. **Built-in moderation** (bans, IP bans, durations, chat filter, AFK) —
   `include/TRaspiConfig.h`, `src/TRaspiConfig.cpp`, `include/TModeration.h`,
   `src/TModeration.cpp`.
2. **Discord relay client** — `include/TDiscordBridge.h`,
   `src/TDiscordBridge.cpp`, plus the separate `relay/` service.

## 1. Turn a feature off (no code changes)

Both features cost **zero runtime resources when disabled**: no thread, socket,
queue or allocation is created, so there is nothing to unload.

```toml
# RaspiConfig.toml
[Moderation]
Enabled = false

[Discord]
Enabled = false
```

`discord enabled but not started` in `discord` console output means it is
configured but idle. Delete `RaspiBans.json` to clear stored bans.

## 2. Remove the Discord bridge only

```bash
rm -rf relay                      # central service (bot + HTTP relay)
rm -f  include/TDiscordBridge.h src/TDiscordBridge.cpp
```

Then drop these references:

- `CMakeLists.txt`: the `src/TDiscordBridge.cpp` line.
- `include/Common.h`: `class TDiscordBridge;` and `static TDiscordBridge& Discord();`
- `src/main.cpp`: the `#include "TDiscordBridge.h"` line and the
  `Application::Discord()` configure/start/shutdown block.
- `src/TServer.cpp`: the `#include "TDiscordBridge.h"` line and the
  `Application::Discord().OnChat(...)` line.
- `src/TConsole.cpp` / `include/TConsole.h`: `Command_Discord` and its
  `mCommandMap` entry plus the help line.
- `include/TRaspiConfig.h` and `src/TRaspiConfig.cpp`: the `TDiscordSettings`
  struct, its parsing block and its `[Discord]` output block.

Removing `[Discord]` from the config is optional: an unused section is ignored.

## 3. Remove the moderation features

Same idea: delete the four moderation files, remove `src/TModeration.cpp` and
`src/TRaspiConfig.cpp` from `CMakeLists.txt`, and remove the `Moderation()`
hooks in `Common.h`, `main.cpp`, `TConsole.*`, `TServer.cpp`, `TNetwork.cpp`
and `TPPSMonitor.cpp`. Because everything is routed through a single
`Application::Moderation()` accessor, that accessor is the only hard dependency
in the rest of the codebase.

## 4. Remove the build dependencies (vcpkg and submodules)

Initialising the submodules is fully reversible; nothing global is installed.

```bash
# un-register the submodules and empty their working trees
git submodule deinit -f deps/commandline deps/toml11 deps/vcpkg

# remove build output and the downloaded vcpkg packages
rm -rf bin
rm -rf vcpkg/downloads vcpkg/buildtrees vcpkg/packages vcpkg/installed
```

To restore them later:

```bash
git submodule update --init --recursive
```

The relay service keeps its data in `relay/data/` and needs `relay/node_modules/`
only for `discord.js`; both are git-ignored and safe to delete:

```bash
rm -rf relay/node_modules relay/data
```
