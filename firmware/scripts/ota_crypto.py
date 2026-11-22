"""Shared OTA helpers: key handling, fingerprint and image encryption.

Web OTA file format: 16-byte IV, then the image encrypted with AES-256-CTR
(matches firmware/main/web_server.c). The key is 64 hex characters. Nothing in
this module prints or logs the key.
"""

import hashlib
import os
import re
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

OTA_MAX_IMAGE = 0x380000  # size of the ota_0 / ota_1 partitions (firmware/partitions.csv)
IV_LEN = 16

_KEY_RE = re.compile(r"[0-9a-f]{64}")


def normalize_key(text: str) -> str:
    """Strip whitespace, lower-case and validate a 64-hex-character key."""
    key = text.strip().lower()
    if not _KEY_RE.fullmatch(key):
        raise ValueError("key must be 64 hex characters")
    return key


def read_key(env: "str | None", file: "str | None") -> str:
    """Read the key from an environment variable or a file (exactly one of them)."""
    if (env is None) == (file is None):
        raise ValueError("give exactly one of --key-env / --key-file")
    if env is not None:
        value = os.environ.get(env)
        if value is None:
            raise ValueError(f"environment variable {env} is not set")
        return normalize_key(value)
    return normalize_key(Path(file).read_text(encoding="utf-8-sig"))


def key_fingerprint(key_hex: str) -> str:
    """First 8 lower-case hex characters of SHA-256 over the raw 32-byte key."""
    return hashlib.sha256(bytes.fromhex(normalize_key(key_hex))).hexdigest()[:8]


def is_zero_key(key_hex: str) -> bool:
    return not any(bytes.fromhex(normalize_key(key_hex)))


def encrypt_bytes(data: bytes, key_hex: str, iv: "bytes | None" = None) -> bytes:
    """Return iv(16) + AES-256-CTR(data). A random IV is used when none is given."""
    if len(data) > OTA_MAX_IMAGE:
        raise ValueError(f"image too large: {len(data)} bytes (partition holds {OTA_MAX_IMAGE})")
    key = bytes.fromhex(normalize_key(key_hex))
    if iv is None:
        iv = os.urandom(IV_LEN)
    if len(iv) != IV_LEN:
        raise ValueError("IV must be 16 bytes")
    enc = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    return iv + enc.update(data) + enc.finalize()
