"""Per-adapter settings, read from <config>/idiff-bridge/<name>.toml.

Values may also come from IDIFF_BRIDGE_<NAME>_<KEY> environment variables,
upper-cased, which is how a secret is usually supplied.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict

from .errors import BridgeError
from .loader import _parse_toml  # same TOML reader, same error text


def config_root() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "idiff-bridge"


def settings_path(adapter: str) -> Path:
    return config_root() / f"{adapter}.toml"


def load_settings(adapter: str, schema: Dict[str, Dict[str, Any]],
                  require: bool = True) -> Dict[str, Any]:
    """Settings for `adapter`; missing required entries raise.

    `require=False` returns what is present, for commands such as doctor
    that exist to report the gap.
    """
    values: Dict[str, Any] = {}
    path = settings_path(adapter)
    if path.is_file():
        doc = _parse_toml(path)
        for k, v in doc.items():
            if isinstance(v, dict) and "value" in v:
                values[k] = v["value"]
            else:
                values[k] = v
    for key in schema:
        env = "IDIFF_BRIDGE_" + adapter.upper() + "_" + key.upper()
        if os.environ.get(env):
            values[key] = os.environ[env]
    if require:
        missing = [k for k, spec in schema.items()
                   if spec.get("required") and not values.get(k)]
        if missing:
            raise BridgeError(
                f"{adapter}: missing setting(s) "
                + ", ".join(missing)
                + f" in {path}")
    return values
