"""Tiny ThingsBoard REST helper (stdlib only).

The tenant-admin password is never stored or printed: it comes from env
TB_PASSWORD or a getpass prompt typed by the user.
"""
import getpass
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_URL = "http://192.168.1.51:8080"
NULL_UUID = "13814000-1dd2-11b2-8080-808080808080"  # ThingsBoard "no customer" id


class TbError(Exception):
    pass


def _decode_attr_value(value):
    if isinstance(value, str) and value[:1] in ("{", "["):
        try:
            return json.loads(value)
        except ValueError:
            pass
    return value


def utf8_stdout():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8")
        except Exception:
            pass


def add_common_args(ap):
    ap.add_argument("--url", default=DEFAULT_URL, help="ThingsBoard base URL (default %(default)s)")
    ap.add_argument("--user", default=os.environ.get("TB_USER"),
                    help="tenant admin e-mail (or env TB_USER); password: env TB_PASSWORD or prompt")


def connect_from_args(args, ap):
    if not args.user:
        ap.error("--user is required (or set env TB_USER)")
    password = os.environ.get("TB_PASSWORD")
    if not password:
        password = getpass.getpass("ThingsBoard password for %s: " % args.user)
    api = TbApi(args.url, args.user, password)
    try:
        api.login()
    except TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        sys.exit(1)
    return api


class TbApi:
    def __init__(self, base_url, user, password, timeout=20):
        self.base = base_url.rstrip("/")
        self.user, self._password, self.timeout = user, password, timeout
        self.token = None

    def login(self):
        res = self.request("POST", "/api/auth/login", {"username": self.user, "password": self._password}, auth=False)
        self._password = None  # not needed any more
        self.token = res.get("token") if isinstance(res, dict) else None
        if not self.token:
            raise TbError("login returned no token")

    def request(self, method, path, body=None, params=None, auth=True, ok404=False):
        url = self.base + path
        if params:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if auth:
            headers["X-Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                raw = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404 and ok404:
                return None
            text = e.read().decode("utf-8", "replace")[:600]
            # never echo the request body (login carries the password)
            raise TbError("%s %s -> HTTP %d: %s" % (method, path, e.code, text)) from None
        except (urllib.error.URLError, OSError) as e:
            raise TbError("%s %s -> cannot reach ThingsBoard: %s" % (method, path, e)) from None
        if not raw:
            return None
        try:
            return json.loads(raw)
        except ValueError:
            return raw.decode("utf-8", "replace")

    def get(self, path, params=None, **kw):
        return self.request("GET", path, params=params, **kw)

    def post(self, path, body=None, params=None, **kw):
        return self.request("POST", path, body, params, **kw)

    def delete(self, path, **kw):
        return self.request("DELETE", path, **kw)

    def rpc_twoway(self, device_id, method, params, timeout_ms=10000):
        """Two-way RPC to a device: returns (http_status, body_obj_or_text). HTTP errors are returned, not raised
        (a timeout is 408/504 and the caller decides). Status 0 = could not reach ThingsBoard."""
        url = "%s/api/plugins/rpc/twoway/%s" % (self.base, device_id)
        payload = json.dumps({"method": method, "params": params, "timeout": timeout_ms}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, method="POST", headers={
            "Accept": "application/json", "Content-Type": "application/json",
            "X-Authorization": "Bearer " + self.token})
        try:
            with urllib.request.urlopen(req, timeout=timeout_ms / 1000.0 + 10) as r:
                status, raw = r.status, r.read()
        except urllib.error.HTTPError as e:
            status, raw = e.code, e.read()
        except (urllib.error.URLError, OSError) as e:
            return 0, "cannot reach ThingsBoard: %s" % e
        text = raw.decode("utf-8", "replace")
        try:
            return status, json.loads(text)
        except ValueError:
            return status, text

    def client_attributes(self, device_id, keys):
        """Client-scope attributes of a device: {key: value}. A value that is a JSON-encoded object or array
        (a string starting with { or [) is decoded; keys ThingsBoard does not have are absent."""
        rows = self.get("/api/plugins/telemetry/DEVICE/%s/values/attributes/CLIENT_SCOPE" % device_id,
                        {"keys": ",".join(keys)})
        return {row["key"]: _decode_attr_value(row.get("value")) for row in (rows or []) if "key" in row}

    def latest_telemetry(self, device_id, keys):
        """Latest telemetry of a device: {key: (ts_ms, value)}; keys with no data are absent."""
        res = self.get("/api/plugins/telemetry/DEVICE/%s/values/timeseries" % device_id, {"keys": ",".join(keys)})
        out = {}
        for key, points in (res or {}).items():
            if points:
                out[key] = (points[0]["ts"], points[0]["value"])
        return out

    def find_by_name(self, list_path, name, key="name", extra=None):
        """Page through a ThingsBoard list endpoint and return the entity whose `key` equals name."""
        page = 0
        while True:
            params = {"pageSize": 100, "page": page, "textSearch": name}
            if extra:
                params.update(extra)
            res = self.get(list_path, params)
            for item in res.get("data", []):
                if item.get(key) == name:
                    return item
            if not res.get("hasNext"):
                return None
            page += 1
