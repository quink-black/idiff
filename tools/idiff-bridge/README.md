# idiff-bridge

Adapts idiff to one external service per adapter. The framework loads a
comparison config into idiff, tells an adapter which group the window
shows, and starts the commands behind the buttons in idiff's actions
panel. idiff stays a general-purpose comparison tool; every
service-specific decision lives in an adapter.

    idiff-bridge://<adapter>/<action>?<query>   link from a service's page
    idiff-bridge run <adapter> <action> --param k=v
    idiff-bridge adapters | doctor <name> | current | advance

Requires Python 3.11 or newer (adapter.toml is read with `tomllib`).
The framework only uses the standard library.

## What an adapter is

Three files in `<config>/idiff-bridge/adapters/<name>`:

| File | Contents |
|---|---|
| `adapter.toml` | name, entry class, settings, panel buttons, argument types |
| `adapter.py` | the Adapter class: one `@action` method per operation |
| `test_adapter.py` | tests against `idiff_bridge.testing`'s fakes |

`template/` is a complete working adapter; `idiff-bridge new <name>` copies
it. `SKILL.md` teaches a coding agent to write one.

## How an action reaches idiff

`ctx.show_config(groups, group)` writes a comparison-config JSON, calls
`comparison_config.load`, switches to the named group, and records the
group name to `Group.meta` mapping in a session file under
`$XDG_CACHE_HOME/idiff-bridge`. `ctx.current_group()` later reads
`state.get`, takes the active `config:<name>` key, and returns the meta
from that record. A verdict therefore names the object on the service
without idiff knowing anything about the service.

idiff's session buttons come from the `actions.set` RPC method; the
`[[buttons]]` in the manifest are sent there by `show_config`. A click
runs `idiff-bridge <adapter> <action>` again with `IDIFF_PID` set, which
is how the action finds the window that invoked it.

## Images

idiff resolves a config entry's path against the directory holding the
config file and up to three parent directories, and downloads only when
none of them has the file. An absolute path such as `/Volumes/share/a.png`
therefore resolves in place when one of those directories is `/`. The
default config location, `/tmp/idiff-bridge-config.json`, is shallow enough
for that; configs written deeper than three levels would
fall back to downloading, which copies the images to the local disk.

## macOS link handler

`idiff-bridge install-url-handler` compiles `~/Applications/idiff-bridge-link.app`,
registers the `idiff-bridge` scheme, and passes each link to `idiff-bridge
url`. Reinstall after moving this directory: the CLI path is baked in.

## Tests

    python3 -m unittest discover -s tests          # framework
    cd tests && python3 -m unittest               # same

Adapter tests live with the adapter; `idiff-bridge new` produces one that
already passes.

## Layout

    idiff-bridge          launcher (adds this directory to sys.path)
    idiff_bridge/         framework
    template/             copy-me adapter
    tests/                framework tests
    SKILL.md              instructions for a coding agent
