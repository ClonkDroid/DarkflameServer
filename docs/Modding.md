# DLU drop-in mods

This fork adds an experimental server-side Lua mod API. Version 1 is intentionally small: it proves that gameplay extensions can be dropped into a Darkflame deployment without recompiling the server for each mod.

## Installing and reloading a mod

Place a file ending in `.dlumod` in the WorldServer mod directory. By default this is `mods/` beside the server binaries. Set `DLU_MODS_DIR` to override it.

Docker Compose mounts `${HOST_MODS_DIR:-./mods}` at `/app/mods` read-only.

Each non-zero WorldServer loads mods at startup. Developers may then reload the complete mod set without restarting the WorldServer:

```text
/modreload
```

Alias: `/reloadmods`.

Reload is transactional. Darkflame first loads every candidate `.dlumod` into fresh isolated Lua states and validates manifests, API versions, duplicate mod IDs, and duplicate mod command aliases. Only when the entire candidate set succeeds is it activated. If any file fails, the currently running mod set remains active and unchanged.

Mod slash commands use stable dispatcher callbacks. Dispatchers never retain a raw pointer to a Lua runtime; they resolve the active runtime/function binding when the command executes. This means replacing or removing a mod cannot leave a slash command callback pointing at a freed Lua state.

A command alias that has existed previously may leave a harmless dispatcher registered after the providing mod is removed. Calling such an alias reports that the command is inactive. This is a deliberate interim tradeoff until the upstream command registry has first-class dynamic command removal/metadata refresh.

## File format

A version-1 `.dlumod` is a single Lua file. It begins with a manifest:

```lua
dlu.mod {
    id = "example",
    name = "Example Mod",
    version = "0.1.0",
    api = 1
}
```

A mod may then register commands:

```lua
dlu.command {
    name = "hello",
    aliases = { "hi" },
    help = "Say hello",
    info = "Example mod command",
    gm_level = 8,
    run = function(args)
        dlu.feedback("Hello from Lua: " .. args)
    end
}
```

All command definitions are collected while the Lua file is evaluated and only become active after the entire candidate mod set validates successfully.

## Management commands

- `/mods` or `/modlist` - list currently active mods
- `/modreload` or `/reloadmods` - transactionally reload every `.dlumod` from the configured mod directory

Both management commands require Developer GM level.

## API version 1

### Metadata and commands

- `dlu.api_version` - currently `1`
- `dlu.mod(table)` - declare the mod manifest
- `dlu.command(table)` - register a slash command
- `dlu.feedback(text)` - send slash-command feedback to the invoking player

### Player/debug operations

These are deliberately thin wrappers around existing, proven Darkflame developer operations:

- `dlu.set_level(level)`
- `dlu.give_item(lot, count)`
- `dlu.set_coins(amount)`
- `dlu.refill()`
- `dlu.lookup(query)`
- `dlu.position()`

### World/entity operations

- `dlu.spawn(lot)`
- `dlu.spawn_group(lot, count, radius)`
- `dlu.test_map(zone, clone)`

## Sandbox status

Every mod gets its own Lua state. Direct filesystem/process/native-module access is removed from the global environment (`io`, `os`, `package`, `require`, `dofile`, `loadfile`, and `debug`).

This is a useful isolation boundary for normal mods, but **API v1 is not a hardened sandbox for hostile code**. Only install mods you trust.

## Bundled Debug World mod

`mods/DebugWorld.dlumod` is the first real consumer of the API.

It adds:

- `/debugworld` or `/dw` - transfer to zone `1100`, clone `9001`, reserved as the debug instance
- `/debug help`
- `/debug level <n>`
- `/debug give <lot> [count]`
- `/debug spawn <lot> [count] [radius]`
- `/debug coins <amount>`
- `/debug heal`
- `/debug lookup <query>`
- `/debug pos`
- `/debug world`

The debug instance reuses an existing LEGO Universe zone and existing client assets; it introduces no new textures, models, map files, or UI resources.

## Next API work

1. Event hooks (`player_login`, `player_smashed`, `entity_spawned`, mission/item events, etc.).
2. Stable `PlayerHandle`, `EntityHandle`, and `World` objects instead of command wrappers.
3. Timers/schedulers.
4. Per-mod persistent storage and configuration.
5. First-class dynamic command removal/metadata refresh in the core command registry.
6. Dependencies/capabilities and bundle-style `.dlumod` archives.
7. NexusDashboard mod management.
