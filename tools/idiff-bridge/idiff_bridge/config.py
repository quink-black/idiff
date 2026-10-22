"""The config an adapter describes, and how it reaches idiff.

An adapter builds Group/Item objects; the framework turns them into the
comparison-config JSON idiff reads, and keeps the metadata that only the
adapter understands in the session record instead of sending it to idiff.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from .errors import BridgeError


@dataclass
class Item:
    url: str
    title: str = ""
    description: str = ""


@dataclass
class Group:
    name: str
    items: List[Item] = field(default_factory=list)
    description: str = ""
    meta: Dict[str, Any] = field(default_factory=dict)


# idiff keys the active group by name ("config:<name>"), so a duplicated
# name would make the session record ambiguous.  Renaming keeps the
# adapter's data intact and reports nothing.
def _uniquify(groups: List[Group]) -> None:
    seen: Dict[str, int] = {}
    for g in groups:
        count = seen.get(g.name, 0)
        seen[g.name] = count + 1
        if count:
            g.name = f"{g.name} ({count + 1})"


def write_config(path: Path, groups: List[Group]) -> None:
    """Write `groups` as a comparison config idiff can load."""
    if not groups:
        raise BridgeError("the config has no groups")
    _uniquify(groups)
    doc = {"groups": []}
    for g in groups:
        entry: Dict[str, Any] = {"name": g.name, "items": []}
        if g.description:
            entry["description"] = g.description
        for it in g.items:
            item: Dict[str, Any] = {"url": it.url}
            if it.title:
                item["title"] = it.title
            if it.description:
                item["description"] = it.description
            entry["items"].append(item)
        doc["groups"].append(entry)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, ensure_ascii=False, indent=1),
                    encoding="utf-8")


def group_meta(groups: List[Group]) -> Dict[str, Dict[str, Any]]:
    """Name -> meta of every group, for the session record."""
    return {g.name: g.meta for g in groups if g.meta}


def find_group(groups: List[Group], name: Optional[str],
               position: int) -> int:
    """Index of the group an action should open on.

    A name wins over a position; without either, the first group is used.
    """
    if name is None:
        if position < 0 or position >= len(groups):
            raise BridgeError(
                f"group index {position} is out of range (have {len(groups)})")
        return position
    for i, g in enumerate(groups):
        if g.name == name:
            return i
    raise BridgeError(f"no group named {name!r} in this config")
