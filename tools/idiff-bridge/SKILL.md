---
name: idiff-bridge
description: |
  Write and drive idiff-bridge adapters. An adapter connects the idiff
  image-comparison GUI to one external service: it loads a batch of
  images into idiff with one link, puts buttons for verdicts into idiff's
  actions panel, and sends each verdict back to the service. Use when the
  user wants idiff to work with their own review service, asks for a new
  adapter, or mentions idiff-bridge, actions.json or idiff-bridge:// links.
---

# idiff-bridge

`idiff-bridge` is a small CLI plus a framework in this directory. Each
adapter is three files; the template in `template/` is a working adapter
whose two actions are `open` (loads a config from the service) and `mark`
(posts a verdict, then moves to the next group).

## Commands

    ./idiff-bridge adapters              # installed adapters, their actions
    ./idiff-bridge new <name>            # copy the template, rename it
    ./idiff-bridge doctor <name>         # settings, server, idiff reachable?
    ./idiff-bridge run <name> <action> --param key=value ...
    ./idiff-bridge url <idiff-bridge://...>
    ./idiff-bridge current               # group idiff shows, with its meta
    ./idiff-bridge advance --step 1
    ./idiff-bridge install-url-handler   # macOS only

Every command prints one JSON document on stdout. Adapters live in
`<config>/idiff-bridge/adapters/<name>` (`$XDG_CONFIG_HOME` or
`~/.config`), or `$IDIFF_BRIDGE_ADAPTERS`; each adapter's settings are in
`<config>/idiff-bridge/<name>.toml`.

## Writing an adapter

1. `./idiff-bridge new myservice`, then open `adapter.py`.
2. Answer the five questions in the template's README. The answers decide
   which actions exist, what goes in `Group.meta`, the `[[buttons]]`, and
   the `[params.*]` types.
3. Implement the service call in `_groups_from` (or a neighbour of it)
   and in the verdict action. Use `ctx.http.get_json` /
   `ctx.http.post_json`; raise `BridgeError` with the server's message
   when they fail.
4. Register the verdict buttons by listing them under `[[buttons]]` in
   `adapter.toml`; `ctx.show_config()` sends them to idiff's actions panel.
5. Test with `FakeIdiff` / `FakeService` from `idiff_bridge.testing`.
   `FakeIdiff(auto_load=True)` makes `comparison_config.load` fill the
   group list from the config file the adapter wrote.
6. `./idiff-bridge doctor myservice` until every check is ok, then run
   `./idiff-bridge url 'idiff-bridge://myservice/open?batch=b1'` with a
   real idiff window open.

## Context API

`ctx.show_config(groups, group=None, index=0)`, `ctx.current_group()`,
`ctx.advance(step)`, `ctx.notify(text)`, `ctx.http`, `ctx.settings`,
`ctx.config_file(template)`, `ctx.idiff.call(method, params)`,
`ctx.register_buttons()`. Types: `Group(name, items, description, meta)`,
`Item(url, title, description)`.

## Rules

- `@action(url=True)` only for read-only actions: any web page can open a
  link.
- Never hard-code the service address or token; they come from settings or
  `IDIFF_BRIDGE_<NAME>_<KEY>`.
- Never print to stdout from an adapter; stdout carries the CLI's JSON.
- Images are passed to idiff as paths idiff can open, typically local
  paths under a mount. The config file must sit in the directory the
  relative paths are relative to, or the paths must be absolute.
- A change to idiff itself is warranted only when a user with no service
  would want it too; otherwise the adapter does the work.
