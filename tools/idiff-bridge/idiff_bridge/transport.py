"""JSON-RPC access to idiff, built on tools/idiffctl/idiff_client.py.

That module owns the wire format and instance discovery for every Python
client of idiff, on POSIX and Windows alike.  This one adds the two things
a bridge needs on top: a retry for a connection idiff closed meanwhile,
and a single rule for choosing the instance an action drives.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Optional

_IDIFFCTL = Path(__file__).resolve().parent.parent.parent / "idiffctl"
if str(_IDIFFCTL) not in sys.path:
    sys.path.insert(0, str(_IDIFFCTL))

import idiff_client  # noqa: E402
from idiff_client import (IdiffConnectionError, IdiffRpcError,  # noqa: E402
                          Instance, discover_instances, probe_socket,
                          socket_path_for_pid)

from .errors import BridgeError  # noqa: E402

__all__ = ["Client", "IdiffConnectionError", "IdiffRpcError", "Instance",
           "discover", "probe", "instance_for_pid"]


class Client(idiff_client.IdiffClient):
    """IdiffClient that reopens a connection the peer already closed."""

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        try:
            return super().call(method, params)
        except (BrokenPipeError, ConnectionResetError):
            # Liveness shows only on write, so a request that failed here
            # was never delivered and can be sent again.
            self.close()
            return super().call(method, params)


def probe(path: str) -> Optional[Instance]:
    return probe_socket(path)


def instance_for_pid(pid: int) -> Optional[Instance]:
    return probe_socket(socket_path_for_pid(pid))


def discover(preferred_pid: Optional[int] = None) -> Instance:
    """The instance an action should drive.

    `preferred_pid` (normally IDIFF_PID) wins when that process answers.
    Without it, or once it has exited, the only live instance is used.
    Several live instances without a usable preferred pid is an error:
    guessing would drive the wrong window.
    """
    if preferred_pid is not None:
        found = instance_for_pid(preferred_pid)
        if found is not None:
            return found
    live = discover_instances()
    if not live:
        raise BridgeError("no running idiff instance")
    if len(live) > 1:
        raise BridgeError(
            "several idiff instances are running ("
            + ", ".join(str(i) for i in live)
            + "); run the action from the idiff window it is meant for, "
              "or set IDIFF_PID")
    return live[0]
