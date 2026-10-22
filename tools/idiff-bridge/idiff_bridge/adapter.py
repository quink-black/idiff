"""Adapter base class and the @action decorator.

An adapter is a class with a name, an optional set of named settings, and
methods marked with @action.  Every method takes the Context as its first
argument; the remaining keyword arguments come from the command line
(`--verdict better`) and are converted according to the adapter's
adapter.toml.
"""

from __future__ import annotations

import inspect
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional


@dataclass
class ActionSpec:
    """One @action method.  Raises TypeError when the adapter class does
    not define the method with 'self' as its first parameter."""

    name: str
    func: Callable[..., Any]
    url: bool = False
    doc: str = ""

    def __get__(self, instance: Any, owner: Any = None) -> Any:
        if instance is None:
            return self
        return _BoundAction(self, instance)


class _BoundAction:
    """What `adapter.open` yields: the spec plus its bound function."""

    def __init__(self, spec: ActionSpec, instance: Any) -> None:
        self.spec = spec
        self.instance = instance

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        return self.spec.func(self.instance, *args, **kwargs)


class Adapter:
    """Base class for adapters.

    Subclasses set `name` and declare actions.  They must not print to
    stdout: the CLI writes exactly one JSON document there.
    """

    name: str = ""

    @classmethod
    def actions(cls) -> Dict[str, ActionSpec]:
        return collect_actions(cls)

    def settings_schema(self) -> Dict[str, Dict[str, Any]]:
        return dict(getattr(self, "settings", {}))


def action(func: Optional[Callable[..., Any]] = None,
           *, url: bool = False) -> Any:
    """Marks a method as runnable as `idiff-bridge <adapter> <name>`.

    `url=True` also lets a `idiff-bridge://` link trigger it.  Keep that
    flag off anything that changes external state: any web page can send
    the link.
    """

    def wrap(f: Callable[..., Any]) -> ActionSpec:
        params = list(inspect.signature(f).parameters)
        if not params or params[0] != "self":
            raise TypeError(f"action {f.__name__} must take self")
        return ActionSpec(name=f.__name__, func=f, url=url,
                          doc=(f.__doc__ or "").strip())

    if func is not None:
        return wrap(func)
    return wrap


def collect_actions(cls: type) -> Dict[str, ActionSpec]:
    """Every @action method on `cls`, subclasses included, by name."""
    found: Dict[str, ActionSpec] = {}
    for name in dir(cls):
        attr = inspect.getattr_static(cls, name, None)
        if isinstance(attr, ActionSpec):
            found[attr.name] = attr
    return found


@dataclass
class Manifest:
    """adapter.toml, parsed."""

    name: str = ""
    entry: str = ""
    settings: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    params: Dict[str, Dict[str, Dict[str, Any]]] = field(default_factory=dict)
    buttons: List[Dict[str, Any]] = field(default_factory=list)
    notes: str = ""
