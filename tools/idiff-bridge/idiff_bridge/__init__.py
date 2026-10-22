"""idiff-bridge -- adapters that drive idiff and talk to an external service.

The framework starts idiff, loads a comparison config written from an
adapter's description, reports which group the window shows, and lets an
adapter send the user's verdicts back to its service.  idiff itself knows
nothing about any of this.

Layout:

  transport.py   JSON-RPC client for idiff (length-prefixed, Unix socket)
  session.py     per-idiff record of the config a bridge loaded into it
  config.py      Group/Item and the comparison-config writer
  adapter.py     Adapter base class and the @action decorator
  context.py     object handed to actions: idiff, config, http, notify
  loader.py      finds adapters, reads adapter.toml, coerces arguments
  testing.py     fake idiff and fake service for adapter tests
  cli.py         command-line entry point
"""

from .adapter import Adapter, Manifest, action
from .config import Group, Item
from .context import Context, Service
from .errors import BridgeError, ResourceNotFound

__all__ = [
    "Adapter", "BridgeError", "Context", "Group", "Item", "Manifest",
    "ResourceNotFound", "Service", "action",
]
