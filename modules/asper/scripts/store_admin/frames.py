"""Bounded AEV2/AEV3 reads shared by offline source and snapshot inspection."""
from contextlib import contextmanager
import hashlib
import io
import os
import re
import stat
import uuid
from .files import check_owner, identity, mount_id, opened

ID = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")
MAX_EVENT = 16 * 1024**2


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def event(stream, limit=MAX_EVENT):
    header = stream.readline(352)
    if not header:
        return None
    try:
        fields = header.decode("ascii").removesuffix("\n").split(" ")
        v3 = len(fields) == 11 and fields[0] == "AEV3"
        if not header.endswith(b"\n") or not (v3 or (len(fields) == 10 and fields[0] == "AEV2")):
            raise ValueError("invalid event header")
        # AEV3 adds the event origin after pinned; AEV2 means origin 0 (host).
        base = fields[:5] + fields[6:] if v3 else fields
        origin_code = int(fields[5]) if v3 else 0
        seq, at, kind, pinned, obj, size = (int(base[i]) for i in (1, 2, 3, 4, 6, 7))
        if (not 1 <= seq <= 2**64-1 or not -(2**63) <= at <= 2**63-1 or
                not 0 <= kind <= 7 or pinned not in (0, 1) or obj not in (0, 71) or
                not 0 <= size <= limit or not ID.fullmatch(base[5]) or
                (v3 and not 1 <= origin_code <= 6)):
            raise ValueError("invalid event fields or length")
        if v3:
            prefix = f"AEV3 {seq} {at} {kind} {pinned} {origin_code} {base[5]} {obj} {size}".encode("ascii")
        else:
            prefix = f"AEV2 {seq} {at} {kind} {pinned} {base[5]} {obj} {size}".encode("ascii")
        width = 9 if v3 else 8
        if (b" ".join(header[:-1].split(b" ")[:width]) != prefix or
                fields[width] != hashlib.sha256(prefix).hexdigest()):
            raise ValueError("event metadata checksum mismatch")
        payload = stream.read(obj + size)
        if (len(payload) != obj + size or stream.read(1) != b"\n" or
                fields[width + 1] != hashlib.sha256(prefix + payload).hexdigest()):
            raise ValueError("event payload checksum or length mismatch")
        origin = payload[:obj].decode("ascii")
        text = payload[obj:].decode("utf-8")
        if "\0" in text or (origin and not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", origin)):
            raise ValueError("invalid source payload")
        return {"id": base[5], "sequence": seq, "at": at, "kind": kind,
                "pinned": pinned, "object_ref": origin, "text": text,
                "origin": origin_code}
    except UnicodeError as error:
        raise ValueError("invalid event encoding") from error


@contextmanager
def file(root, name, limit):
    with opened(name, root) as fd:
        before = os.fstat(fd)
        check_owner(before)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                before.st_size > limit or mount_id(fd) != mount_id(root)):
            raise ValueError("invalid or oversized source file")
        with os.fdopen(os.dup(fd), "rb") as stream:
            yield stream
        if (identity(os.fstat(fd)) != identity(before) or
                identity(os.stat(name, dir_fd=root, follow_symlinks=False)) != identity(before)):
            raise ValueError("source file changed during inspection")


def checked_decode(data, limit):
    stream = io.BytesIO(data)
    row = event(stream, limit)
    if (row is None or row["sequence"] != 1 or row["at"] or row["pinned"] or
            row["kind"] != 5 or row["object_ref"] or stream.read(1)):
        raise ValueError("invalid checked snapshot frame")
    return row["text"]


def checked_encode(text):
    data = text.encode("utf-8")
    prefix = f"AEV2 1 0 5 0 {uuid.uuid4()} 0 {len(data)}".encode("ascii")
    return (prefix + b" " + hashlib.sha256(prefix).hexdigest().encode("ascii") + b" " +
            hashlib.sha256(prefix + data).hexdigest().encode("ascii") + b"\n" + data + b"\n")


def checked_read(root, name, limit):
    with file(root, name, limit + 512) as stream:
        data = stream.read(limit + 513)
        if len(data) > limit + 512:
            raise ValueError("snapshot grew past its limit")
        return checked_decode(data, limit)
