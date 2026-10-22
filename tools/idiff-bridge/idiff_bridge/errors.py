"""Exception types shared by the framework and adapters."""


class BridgeError(Exception):
    """A failure the user can act on.  The CLI prints the message alone."""


class ResourceNotFound(BridgeError):
    """A config or group named in an action is not in the session."""
