"""__NAME__ -- an idiff-bridge adapter.

An adapter is two actions:

  open  reads a config from the service and loads it into idiff.  It is
        reachable from a idiff-bridge:// link, so the service can put
        "open this batch in idiff" buttons on its own pages.
  mark  sends the user's verdict about the group idiff shows back to the
        service.  It is only reachable from the actions panel, never
        from a link: any web page can send a link.

Copy this directory, rename the actions, and replace the two request
helpers below with your service's API.  Everything else -- settings,
buttons, link handling, error reporting -- stays as it is.
"""

from __future__ import annotations

from idiff_bridge import Adapter, BridgeError, Group, Item, action


class Adapter(Adapter):
    """Replace the bodies below; keep the signatures.

    `settings` mirrors [settings] in adapter.toml and is used only when
    the manifest cannot be read; the CLI normally takes the schema from
    the manifest.
    """

    settings = {"server": {"required": True}, "token": {"required": False}}

    @action(url=True)
    def open(self, ctx, group: str = "", batch: str = ""):
        """Loads the batch the service names into idiff."""
        doc = ctx.http.get_json("/api/config", {"batch": batch})
        groups = self._groups_from(doc)
        return ctx.show_config(groups, group=group or None)

    @action
    def mark(self, ctx, verdict: str = "reviewed"):
        """Records a verdict about the group idiff shows."""
        current = ctx.current_group()
        ctx.http.post_json("/api/verdict", {
            "case": current["meta"].get("case_id"),
            "verdict": verdict,
        })
        ctx.notify(f"{current['name']}: {verdict}")
        ctx.advance(1)
        return {"case": current["meta"].get("case_id"), "verdict": verdict}

    # -- helpers ------------------------------------------------------
    @staticmethod
    def _groups_from(doc) -> list:
        """Builds Group objects from the service's response.

        Adjust the field names to the service.  `meta` is kept by the
        bridge session only; idiff never sees it, and `current_group()`
        hands it back in `mark`.
        """
        if not isinstance(doc, dict) or "groups" not in doc:
            raise BridgeError("/api/config: response has no 'groups'")
        groups = []
        for g in doc["groups"]:
            try:
                name = str(g["name"])
                items = [Item(url=str(i["path"]), title=str(i.get("title", "")))
                         for i in g["items"]]
            except (KeyError, TypeError) as e:
                raise BridgeError(f"/api/config: malformed group ({e})")
            groups.append(Group(name=name, items=items,
                                description=str(g.get("description", "")),
                                meta=dict(g.get("meta") or {})))
        if not groups:
            raise BridgeError("/api/config: the service returned no groups")
        return groups
