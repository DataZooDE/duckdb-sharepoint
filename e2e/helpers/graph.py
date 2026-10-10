"""A minimal Microsoft Graph client for the harness -- the "other client".

The e2e layer exists for what SQLLogicTest cannot express: a change made
OUTSIDE the DuckDB process under test (another user editing the library),
observed by a DuckDB process that is still running. This client is that other
user. It speaks to Graph directly with app-only (client credentials) auth,
the same principal the extension's secret uses.

Values come from the environment only (SHAREPOINT_TENANT_ID, _CLIENT_ID,
_CLIENT_SECRET, _DRIVE_ID); nothing here is ever written to a tracked file.
"""

from __future__ import annotations

import os
import time
import urllib.parse
from dataclasses import dataclass, field

import requests

GRAPH = "https://graph.microsoft.com/v1.0"
NEEDED = ("SHAREPOINT_TENANT_ID", "SHAREPOINT_CLIENT_ID", "SHAREPOINT_CLIENT_SECRET", "SHAREPOINT_DRIVE_ID")


class GraphConfigError(RuntimeError):
    pass


def credentials_available() -> bool:
    return all(os.environ.get(n) for n in NEEDED)


def _quote_path(path: str) -> str:
    return urllib.parse.quote(path.strip("/"), safe="/")


@dataclass
class Graph:
    tenant: str
    client_id: str
    client_secret: str = field(repr=False)
    drive_id: str
    _token: str = field(default="", repr=False)
    _expires: float = 0.0

    @classmethod
    def from_env(cls) -> "Graph":
        missing = [n for n in NEEDED if not os.environ.get(n)]
        if missing:
            raise GraphConfigError(f"missing {', '.join(missing)}")
        return cls(os.environ["SHAREPOINT_TENANT_ID"], os.environ["SHAREPOINT_CLIENT_ID"],
                   os.environ["SHAREPOINT_CLIENT_SECRET"], os.environ["SHAREPOINT_DRIVE_ID"])

    # -- auth ---------------------------------------------------------------
    def token(self) -> str:
        if self._token and time.time() < self._expires - 60:
            return self._token
        r = requests.post(
            f"https://login.microsoftonline.com/{self.tenant}/oauth2/v2.0/token",
            data={"client_id": self.client_id, "client_secret": self.client_secret,
                  "grant_type": "client_credentials", "scope": "https://graph.microsoft.com/.default"},
            timeout=60)
        r.raise_for_status()
        body = r.json()
        self._token = body["access_token"]
        self._expires = time.time() + int(body.get("expires_in", 3600))
        return self._token

    def _req(self, method: str, url: str, **kw) -> requests.Response:
        headers = kw.pop("headers", {})
        headers["Authorization"] = f"Bearer {self.token()}"
        for attempt in range(5):
            r = requests.request(method, url, headers=headers, timeout=120, **kw)
            if r.status_code in (429, 503) and attempt < 4:
                time.sleep(int(r.headers.get("Retry-After", "2")))
                continue
            return r
        return r

    def _item(self, path: str) -> str:
        return f"{GRAPH}/drives/{self.drive_id}/root:/{_quote_path(path)}"

    # -- operations -----------------------------------------------------------
    def stat(self, path: str) -> dict | None:
        r = self._req("GET", self._item(path))
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def upload(self, path: str, data: bytes) -> dict:
        """Single PUT (<= 250 MB). Creates parent folders implicitly."""
        r = self._req("PUT", self._item(path) + ":/content", data=data,
                      headers={"Content-Type": "application/octet-stream"})
        r.raise_for_status()
        return r.json()

    def mkdir(self, path: str) -> dict:
        parent, _, name = path.strip("/").rpartition("/")
        url = (f"{GRAPH}/drives/{self.drive_id}/root/children" if not parent
               else self._item(parent) + ":/children")
        r = self._req("POST", url, json={"name": name, "folder": {},
                                          "@microsoft.graph.conflictBehavior": "replace"})
        r.raise_for_status()
        return r.json()

    def delete(self, path: str, permanent: bool = True) -> bool:
        item = self.stat(path)
        if item is None:
            return False
        url = f"{GRAPH}/drives/{self.drive_id}/items/{item['id']}"
        r = self._req("POST", url + "/permanentDelete") if permanent else self._req("DELETE", url)
        if r.status_code == 404:
            return False
        r.raise_for_status()
        return True

    def children(self, path: str) -> list[dict]:
        url = self._item(path) + ":/children?$top=200" if path.strip("/") else \
            f"{GRAPH}/drives/{self.drive_id}/root/children?$top=200"
        out: list[dict] = []
        while url:
            r = self._req("GET", url)
            if r.status_code == 404:
                return []
            r.raise_for_status()
            body = r.json()
            out.extend(body.get("value", []))
            url = body.get("@odata.nextLink", "")
        return out
