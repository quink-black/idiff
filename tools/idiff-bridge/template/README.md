# Adapter template

Copy this directory to `<config>/idiff-bridge/adapters/<name>` (or run
`idiff-bridge new <name>`), then edit `adapter.toml` and `adapter.py`.
Three files are the whole adapter: the manifest, the code, and its test.

## The five questions

Answering them fixes the shape of the adapter; the template's comments say
where each answer goes.

1. **Who triggers it?** A link from the service's own page, a button in
   idiff's actions panel, or a shell command. Only an action that merely
   *shows* something may carry `@action(url=True)`; any web page can send
   a link.
2. **What does the service return?** Write the endpoint down, then map it
   to `Group`/`Item` in one helper. Keep the mapping in one function.
3. **How does a group map back to the service's object?** Put its id in
   `Group.meta`. idiff never sees meta; `ctx.current_group()["meta"]`
   hands it back, which is what a verdict needs.
4. **Which user actions must reach the service?** Each one is an action,
   each action gets a `[[buttons]]` entry, and its arguments get a
   `[params.<action>.<arg>]` type.
5. **Is something missing from idiff?** Check the RPC methods first
   (`docs/rpc-design.md`). A change belongs in idiff itself only when a
   user with no service would want it too; anything else lives here.

## Checklist before offering the adapter

- [ ] `idiff-bridge doctor <name>` reports every check as ok.
- [ ] `python3 -m unittest discover -s <adapter dir>` passes with the
      checkout on `PYTHONPATH`.
- [ ] Only read-only actions carry `url=True`.
- [ ] The service address and token come from `ctx.settings`, never from
      the code or from a link.
- [ ] Every service error surfaces as `BridgeError` with the status code
      and the server's message.
- [ ] The README of the service repository says how to install the link
      handler (`idiff-bridge install-url-handler`) where needed.
