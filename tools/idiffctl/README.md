# idiffctl

Command-line client for the JSON-RPC server that every running idiff
window hosts (`/tmp/idiff-<pid>.sock` on POSIX, `\\.\pipe\idiff-<pid>`
on Windows). It is meant for scripts and AI agents: every command prints
one JSON document on stdout, errors go to stderr, and the exit status
says what went wrong.

It needs Python 3.8 or newer and nothing outside the standard library,
so there is no virtual environment to set up.

```bash
tools/idiffctl/idiffctl.py open a.png b.png     # starts idiff if none is running
tools/idiffctl/idiffctl.py state --brief
tools/idiffctl/idiffctl.py ref a.png            # paths as `state` reports them
tools/idiffctl/idiffctl.py metrics              # PSNR / SSIM / MSE vs. the reference
tools/idiffctl/idiffctl.py sample 120 80        # pixel values at native x, y
tools/idiffctl/idiffctl.py view --mode difference
tools/idiffctl/idiffctl.py shot /tmp/view.png
tools/idiffctl/idiffctl.py call comparison_config.switch_group '{"group_index": 2}'
```

Run `idiffctl.py --help` and `idiffctl.py COMMAND --help` for every
option. `call` sends any method listed in
[`docs/rpc-design.md`](../../docs/rpc-design.md), so methods without a
dedicated command are still reachable.

## Conventions

- **ENTRY** arguments name a library entry. A decimal number is an
  index; anything else is a path. idiff matches it verbatim against the
  `path` values `state` reports or, failing that, as the same file
  (symlinks and `.` / `..` resolved).
- Relative paths are made absolute before they are sent, because idiff
  resolves paths against its own working directory, not the caller's.
- **Target instance**: `--pid`, else `$IDIFF_PID`, else the only running
  instance. Global options (`--pid`, `--timeout`, `--pretty`) may appear
  before or after the command name.
- **`open`** loads into the target instance, or starts idiff when none is
  running (`--new` always starts one). The executable is `--bin`, else
  `$IDIFF_BIN`, else `idiff` on `PATH`, else `idiff.app` in
  `/Applications` or `~/Applications` (macOS), else this repository's
  `build/src/app/`.

## Exit status

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | idiff rejected the request (JSON-RPC error; message on stderr) |
| 2 | usage error |
| 3 | no idiff instance is running |
| 4 | several instances are running and none was chosen, or the chosen pid is not running |
| 5 | connection failure or timeout (the request may still have taken effect) |

## Agent skill

[`SKILL.md`](SKILL.md) makes this directory a skill for coding agents
that load `SKILL.md` skills, such as CodeBuddy and pi. It tells the agent
when to use idiffctl and how. Install it by symlinking the directory
into the agent's skill directory, from the repository root:

```bash
ln -s "$PWD/tools/idiffctl" ~/.codebuddy/skills/idiffctl
ln -s "$PWD/tools/idiffctl" ~/.pi/agent/skills/idiffctl
```

## Files

- `SKILL.md`: the agent skill.
- `idiff_client.py`: transport, framing and instance discovery. The MCP
  server in `tools/idiff-mcp/` imports it as well.
- `idiffctl.py`: the command-line interface.
- `test_idiffctl.py`: tests against fake servers (POSIX). ctest runs
  them as `idiffctl`; by hand, run `python3 -m unittest test_idiffctl`
  in this directory.
