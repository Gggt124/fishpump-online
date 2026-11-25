"""Minimal MQTT 3.1.1 client (stdlib sockets): CONNECT with username, PUBLISH QoS 0, SUBSCRIBE QoS 0 and
receive PUBLISH (enough for a fake board answering RPCs), DISCONNECT.

Enough to drive ThingsBoard device-API tests. Run `python mqtt_mini.py --selftest`
to check the packet encoding offline.
"""
import socket
import sys

CONNACK_TEXT = {
    0: "accepted",
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad user name or password",
    5: "not authorized",
}


class MqttError(Exception):
    pass


def encode_remaining_length(n):
    if n < 0 or n > 268435455:
        raise ValueError("remaining length out of range")
    out = bytearray()
    while True:
        digit = n % 128
        n //= 128
        if n:
            digit |= 0x80
        out.append(digit)
        if not n:
            return bytes(out)


def _utf8(s):
    b = s.encode("utf-8")
    return len(b).to_bytes(2, "big") + b


def build_connect(client_id, username=None, password=None, keepalive=60, clean=True):
    flags = 0x02 if clean else 0
    payload = _utf8(client_id)
    if username is not None:
        flags |= 0x80
        payload += _utf8(username)
        if password is not None:
            flags |= 0x40
            payload += _utf8(password)
    var = _utf8("MQTT") + bytes([4, flags]) + keepalive.to_bytes(2, "big")
    body = var + payload
    return b"\x10" + encode_remaining_length(len(body)) + body


def build_publish(topic, payload, retain=False):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    body = _utf8(topic) + payload  # QoS 0: no packet id
    return bytes([0x30 | (1 if retain else 0)]) + encode_remaining_length(len(body)) + body


def build_disconnect():
    return b"\xe0\x00"


def build_subscribe(packet_id, topic):
    """SUBSCRIBE one topic filter at QoS 0."""
    body = packet_id.to_bytes(2, "big") + _utf8(topic) + b"\x00"
    return b"\x82" + encode_remaining_length(len(body)) + body


def read_packet(recv):
    """Read one packet with recv(n) -> exactly n bytes. Returns (first_byte, body)."""
    first = recv(1)[0]
    n, shift = 0, 0
    for _ in range(4):
        b = recv(1)[0]
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            break
        shift += 7
    else:
        raise MqttError("malformed remaining length")
    return first, (recv(n) if n else b"")


def parse_publish(first_byte, body):
    """(topic, payload) of a PUBLISH body; QoS 1/2 packets carry a packet id after the topic, which is skipped."""
    tlen = int.from_bytes(body[:2], "big")
    topic = body[2:2 + tlen].decode("utf-8")
    pos = 2 + tlen
    if (first_byte >> 1) & 0x03:
        pos += 2
    return topic, bytes(body[pos:])


def parse_connack(data):
    """Return the CONNACK return code; raise MqttError when it is not a CONNACK."""
    if len(data) != 4 or data[0] != 0x20 or data[1] != 2:
        raise MqttError("not a CONNACK packet: %r" % (bytes(data),))
    return data[3]


class MqttClient:
    def __init__(self, host, port=1883, client_id="mqtt-mini", username=None, password=None,
                 keepalive=60, timeout=5.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.client_id, self.username, self.password, self.keepalive = client_id, username, password, keepalive
        self.sock = None

    def _recv_exact(self, n, waiting_for="CONNACK (ThingsBoard closes the socket on a bad access token)"):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise MqttError("connection closed by broker while waiting for %s" % waiting_for)
            buf += chunk
        return buf

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        self.sock.sendall(build_connect(self.client_id, self.username, self.password, self.keepalive))
        rc = parse_connack(self._recv_exact(4))
        if rc != 0:
            self.close()
            raise MqttError("CONNACK refused, code %d (%s)" % (rc, CONNACK_TEXT.get(rc, "unknown")))

    def publish(self, topic, payload):
        self.sock.sendall(build_publish(topic, payload))

    def _read_packet(self, waiting_for):
        return read_packet(lambda n: self._recv_exact(n, waiting_for))

    def subscribe(self, topic, packet_id=1):
        """SUBSCRIBE at QoS 0 and wait for the SUBACK (PUBLISHes that arrive first are dropped)."""
        self.sock.sendall(build_subscribe(packet_id, topic))
        while True:
            first, body = self._read_packet("SUBACK")
            if first & 0xF0 == 0x90:
                if body[2:3] == b"\x80":
                    raise MqttError("SUBSCRIBE %s refused" % topic)
                return

    def recv_publish(self, timeout):
        """Next PUBLISH as (topic, payload), or None when nothing arrives within timeout seconds.
        Other packet types (PINGRESP...) are skipped."""
        self.sock.settimeout(timeout)
        try:
            while True:
                try:
                    first = self.sock.recv(1)
                except socket.timeout:
                    return None
                if not first:
                    raise MqttError("connection closed by broker while waiting for PUBLISH")
                pending = bytearray(first)

                def recv(n):
                    if pending:
                        out = bytes(pending[:n])
                        del pending[:n]
                        return out + (self._recv_exact(n - len(out), "PUBLISH") if len(out) < n else b"")
                    return self._recv_exact(n, "PUBLISH")
                kind, body = read_packet(recv)
                if kind & 0xF0 == 0x30:
                    return parse_publish(kind, body)
        finally:
            self.sock.settimeout(self.timeout)

    def disconnect(self):
        try:
            if self.sock is not None:
                self.sock.sendall(build_disconnect())
        except OSError:
            pass
        self.close()

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


def selftest():
    failures = []

    def check(name, got, want):
        ok = got == want
        print("%s %s" % ("ok  " if ok else "FAIL", name))
        if not ok:
            print("     got  %r\n     want %r" % (got, want))
            failures.append(name)

    for n, want in ((0, b"\x00"), (127, b"\x7f"), (128, b"\x80\x01"),
                    (16383, b"\xff\x7f"), (16384, b"\x80\x80\x01")):
        check("remaining length %d" % n, encode_remaining_length(n), want)
    check("remaining length max", encode_remaining_length(268435455), b"\xff\xff\xff\x7f")

    want = (b"\x10\x15" b"\x00\x04MQTT" b"\x04\x82" b"\x00\x3c"
            b"\x00\x04abcd" b"\x00\x03tok")
    check("CONNECT id=abcd user=tok", build_connect("abcd", "tok"), want)
    want_pw = (b"\x10\x18" b"\x00\x04MQTT" b"\x04\xc2" b"\x00\x3c"
               b"\x00\x04abcd" b"\x00\x03tok" b"\x00\x01p")
    check("CONNECT with password", build_connect("abcd", "tok", "p"), want_pw)

    check("PUBLISH QoS0", build_publish("a/b", b"hi"), b"\x30\x07\x00\x03a/b" b"hi")
    big = build_publish("t", b"x" * 200)  # remaining length 203 -> two length bytes
    check("PUBLISH long framing", big[:3], b"\x30\xcb\x01")
    check("PUBLISH long size", len(big), 1 + 2 + 203)
    check("DISCONNECT", build_disconnect(), b"\xe0\x00")

    check("CONNACK accepted", parse_connack(b"\x20\x02\x00\x00"), 0)
    check("CONNACK not authorized", parse_connack(b"\x20\x02\x00\x05"), 5)
    try:
        parse_connack(b"\x30\x02\x00\x00")
        check("CONNACK garbage rejected", False, True)
    except MqttError:
        check("CONNACK garbage rejected", True, True)

    check("SUBSCRIBE rpc requests", build_subscribe(1, "v1/devices/me/rpc/request/+"),
          b"\x82\x20\x00\x01\x00\x1bv1/devices/me/rpc/request/+\x00")
    check("PUBLISH parse QoS0", parse_publish(0x30, b"\x00\x03a/b" + b"hi"), ("a/b", b"hi"))
    check("PUBLISH parse QoS1 skips packet id", parse_publish(0x32, b"\x00\x03a/b\x00\x07" + b"hi"), ("a/b", b"hi"))

    def feeder(data):
        buf = bytearray(data)

        def recv(n):
            out = bytes(buf[:n])
            del buf[:n]
            return out
        return recv

    check("read SUBACK packet", read_packet(feeder(b"\x90\x03\x00\x01\x00")), (0x90, b"\x00\x01\x00"))
    long_pub = build_publish("t", b"x" * 200)
    check("read long PUBLISH packet", read_packet(feeder(long_pub)), (0x30, long_pub[3:]))
    check("PUBLISH round trip", parse_publish(*read_packet(feeder(build_publish("a/b", b"hi")))), ("a/b", b"hi"))

    print("SELFTEST %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print("usage: python mqtt_mini.py --selftest")
    sys.exit(2)
