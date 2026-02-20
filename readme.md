# rift.lua

Lua Mach IPC client for Rift WM.

## Build

```bash
cd rift.lua
make
```

Outputs: `rift.lua/bin/rift.so`

## Load

```lua
package.cpath = "./bin/?.so;" .. package.cpath
local rift = require("rift")
```

## Connect

```lua
local client, err = rift.connect()
if not client then error(err) end
```

`client:reconnect()` reconnects and returns the same client object.

## Request/Response API

```lua
local resp, err = client:send_request([[{"get_workspaces":{"space_id":null}}]])
if not resp then error(err) end
```

- Input is raw JSON string.
- Output is decoded Lua table.

## Event Streaming

Supported events:

- `workspace_changed`
- `windows_changed`
- `window_title_changed`
- `stacks_changed`
- `*` (all)

### Callback mode (recommended)

```lua
client:subscribe({ "*" }, function(env)
  -- env.INFO: raw JSON string
  -- env.EVENT: event name (e.g. "windows_changed")
  -- env.DATA: decoded table
  print(env.EVENT)
end)
```

`subscribe(events, callback)` returns immediately and auto-dispatches callbacks.

## Notes

- If you subscribe to `*`, you will receive all Rift broadcast event types listed above.
- Keep your Lua process alive to keep receiving events.

## Acknowledgements

https://github.com/FelixKratz/SbarLua
