"""Tiny HTTP client for the fishpump board's web API (stdlib only).

Logs in the way firmware/main/static/app.js does: POST /api/login with a JSON
body, then keeps the HttpOnly `session` cookie. Every request carries
`Origin: <base_url>` because the board rejects cross-origin POSTs
(is_same_origin in web_server.c). The password is never stored or printed:
it comes from env BOARD_PASSWORD or a getpass prompt typed by the user.
"""
import getpass
import http.cookiejar
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_BOARD = "http://192.168.1.34"
DEFAULT_USER = "admin"


class BoardError(Exception):
    pass


class BoardSession:
    def __init__(self, base_url, user, password, timeout=20):
        self.base = base_url.rstrip("/")
        self.user, self._password, self.timeout = user, password, timeout
        self._jar = http.cookiejar.CookieJar()
        self._opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self._jar))

    def login(self):
        status, text = self.request(
            "POST", "/api/login",
            {"username": self.user, "password": self._password, "remember": False})
        self._password = None  # not needed any more
        try:
            data = json.loads(text)
        except ValueError:
            data = None
        if status != 200 or not isinstance(data, dict) or not data.get("ok"):
            err = data.get("error") if isinstance(data, dict) else None
            raise BoardError("login failed: HTTP %d %s" % (status, err or text[:80]))
        if not any(c.name == "session" for c in self._jar):
            raise BoardError("login ok but the board set no session cookie")
        return data

    def request(self, method, path, body=None):
        """Returns (status, text). HTTP error statuses are returned, not raised.

        body: dict/list -> JSON; None -> an empty body (POST) or none (GET).
        """
        data = None
        headers = {"Origin": self.base}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif method != "GET":
            data = b""
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            with self._opener.open(req, timeout=self.timeout) as resp:
                return resp.status, resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", errors="replace")
        except (urllib.error.URLError, OSError) as e:
            raise BoardError("%s %s: %s" % (method, path, e))


def add_board_args(ap):
    ap.add_argument("--board", default=DEFAULT_BOARD, help="board base URL (default %(default)s)")
    ap.add_argument("--board-user", default=DEFAULT_USER,
                    help="board web user (default %(default)s); password: env BOARD_PASSWORD or prompt")


def board_from_args(args):
    password = os.environ.get("BOARD_PASSWORD")
    if not password:
        password = getpass.getpass("Board password for %s@%s: " % (args.board_user, args.board))
    session = BoardSession(args.board, args.board_user, password)
    try:
        session.login()
    except BoardError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        sys.exit(1)
    return session
