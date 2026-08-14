"""PlatformIO pre-build hook for BoseFix32.

Same contract as in CULFW32 / TCM32 — see /root/.claude/CLAUDE.md global
section "Versioning + lokales Git + Pre-Build-Backup".

For every `pio run` invocation:
  1. If the project is a git repo and the working tree has uncommitted
     changes, snapshot them (`git add -A; git commit`) so we can revert
     to any prior built state. The commit message records the *previous*
     build number — so the commit represents "the state about to be rebuilt".
  2. Increment `build_number.txt`.
  3. Regenerate `src/version.h` with `MAJOR.MINOR.BUILD` and timestamp.

Manual fields:
  - `version.txt`       — `MAJOR.MINOR` (one line, e.g. `0.1`). Bump by hand
    when crossing a meaningful project phase.
  - `build_number.txt`  — auto-incremented; do not edit unless resetting.

This script touches only the project tree. No global git config is modified.
"""

import datetime
import os
import subprocess
import time

Import("env")  # noqa: F821 — injected by PlatformIO

PROJECT_DIR  = env["PROJECT_DIR"]                                       # noqa: F821
VERSION_FILE = os.path.join(PROJECT_DIR, "version.txt")
BUILD_FILE   = os.path.join(PROJECT_DIR, "build_number.txt")
HEADER_FILE  = os.path.join(PROJECT_DIR, "src", "version.h")


def _read_version():
    with open(VERSION_FILE) as f:
        v = f.read().strip()
    parts = v.split(".")
    if len(parts) != 2:
        raise RuntimeError(f"version.txt must contain MAJOR.MINOR — got {v!r}")
    return int(parts[0]), int(parts[1])


def _read_build():
    if not os.path.exists(BUILD_FILE):
        return 0
    with open(BUILD_FILE) as f:
        return int((f.read().strip() or "0"))


def _atomic_write(path, content):
    """Atomic write + fsync — verhindert NULL-bytes-Korruption auf NFS."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    # Auch Directory fsync, damit NFS den Rename committet
    try:
        dfd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
        try: os.fsync(dfd)
        finally: os.close(dfd)
    except OSError:
        pass


def _atomic_write_bytes(path, data):
    """Binary atomic write + fsync (file + dir). Ohne fsync schreibt NFS Bloecke
    verzoegert zurueck -> mklittlefs liest ein halb-gefuelltes File (NUL-Praefix)
    und packt eine kaputte index.html.gz ins LittleFS-Image."""
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    try:
        dfd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
        try: os.fsync(dfd)
        finally: os.close(dfd)
    except OSError:
        pass


def _read_text_source_nfs(path, markers=(b"<html", b"</html>"), retries=6):
    """NFS-safe Read einer Text-Quelldatei (z.B. web-src/index.html).
    Ein einzelner Read ueber NFS kann einen NUL-praefixierten ersten 4-KB-Block
    liefern (lazy write-back race): die Laenge stimmt, der Inhalt ist Muell.
    Validiere den INHALT (keine NUL-Bytes + erwartete Marker, nicht nur Laenge)
    und lies bei Korruption neu — mit Page-Cache-Invalidierung (posix_fadvise
    DONTNEED), damit der Retry frisch vom NFS-Server holt statt den kaputten
    Cache erneut zu sehen. Sonst gzippt der Build stillschweigend eine kaputte
    UI (Vorfall 2026-06-03: web-src/index.html mit 4096 NUL-Bytes am Anfang ->
    servierte Seite ohne <head>/<title>). Der gz-Write-Validate weiter unten
    merkt das NICHT, weil er gegen 'raw' prueft und 'raw' selbst schon korrupt
    ist — deshalb muss die Quelle hier validiert werden."""
    last = b""
    for attempt in range(retries):
        with open(path, "rb") as fi:
            raw = fi.read()
        if raw and b"\x00" not in raw and all(m in raw.lower() for m in markers):
            return raw
        last = raw
        print(f"[nfs-read] {os.path.basename(path)} sieht korrupt aus "
              f"(Versuch {attempt+1}/{retries}: len={len(raw)}, "
              f"NUL={raw.count(bytes(1))}) -> Page-Cache verwerfen + re-read")
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            finally:
                os.close(fd)
        except (AttributeError, OSError):
            pass
        time.sleep(0.4)
    raise RuntimeError(
        f"[nfs-read] {path} blieb nach {retries} Reads korrupt "
        f"(NUL={last.count(bytes(1))} Bytes, len={len(last)}) — NFS-write-back-race "
        f"oder Quelle persistent kaputt. Build abgebrochen, statt eine kaputte UI "
        f"zu gzippen/flashen. Quelle pruefen / aus git wiederherstellen.")


def _write_build(n):
    _atomic_write(BUILD_FILE, f"{n}\n")


def _is_git_repo():
    return os.path.isdir(os.path.join(PROJECT_DIR, ".git"))


def _git_run(args, capture=False):
    return subprocess.run(
        ["git"] + args,
        cwd=PROJECT_DIR,
        capture_output=capture,
        text=True,
        check=False,
    )


def _git_has_changes():
    r = _git_run(["status", "--porcelain"], capture=True)
    return bool(r.stdout.strip())


def _git_autocommit(prev_version_str):
    if not _git_has_changes():
        return False
    add = _git_run(["add", "-A"])
    if add.returncode != 0:
        print(f"[version_bump] git add failed (rc={add.returncode}); skip commit")
        return False
    msg = f"build snapshot v{prev_version_str}"
    # NFS-Flaky-Workaround: a commit-msg hook makes git re-read
    # .git/COMMIT_EDITMSG from the NFS-backed repo after the hook returns.
    # The NFS client intermittently serves a stale/NUL-padded read, so git
    # aborts with "a NUL byte in commit log message not allowed" /
    # "failed to write commit object" — even though the message is always
    # clean ASCII and no commit object is written (so a retry can't dupe).
    # Empirically transient (~1-in-3), so retry a few times. We do NOT pass
    # --no-verify: the hook must keep running; the snapshot just needs to land.
    last = ""
    for attempt in range(8):
        cm = _git_run(["commit", "-m", msg], capture=True)
        if cm.returncode == 0:
            return True
        last = ((cm.stderr or "") + (cm.stdout or "")).strip()
        transient = "NUL byte" in last or "failed to write commit object" in last
        if not transient:
            break  # real failure (e.g. hook rejection) — don't retry
        time.sleep(0.25 * (attempt + 1))
    print(f"[version_bump] git commit skipped: {last}")
    return False


def _last_release_core(major, minor):
    """MAJOR.MINOR.PATCH of the most recent release tag on the current
    MAJOR.MINOR line, else f"{major}.{minor}.0".

    Dev builds report "<core>+<build>" (semver build-metadata: the part after
    '+' does not affect precedence). Because the build counter now lives behind
    the '+', the firmware's semverCompare_ — which reads only the leading
    MAJOR.MINOR.PATCH (sscanf %d stops at '+') — treats a dev build as its base
    release, not as a bogus high patch. Before this, a dev build "0.8.<counter>"
    looked NEWER than a real release "0.8.4" (1105 > 4), so the device's OTA
    check said "no downgrade" and refused the update while serving a stale UI.
    """
    if not _is_git_repo():
        return f"{major}.{minor}.0"
    r = _git_run(["describe", "--tags", "--abbrev=0", "--match", "v*"], capture=True)
    if r.returncode == 0:
        parts = r.stdout.strip().lstrip("v").split(".")
        if len(parts) >= 3 and all(p.isdigit() for p in parts[:3]):
            tmaj, tmin, tpatch = int(parts[0]), int(parts[1]), int(parts[2])
            if (tmaj, tmin) == (major, minor):
                return f"{major}.{minor}.{tpatch}"
    return f"{major}.{minor}.0"


def main():
    major, minor = _read_version()
    prev_build = _read_build()
    new_build  = prev_build + 1
    prev_version = f"{major}.{minor}.{prev_build}"
    new_version  = f"{major}.{minor}.{new_build}"

    # Step 1: snapshot working tree under the *previous* version label.
    if _is_git_repo():
        if _git_autocommit(prev_version):
            print(f"[version_bump] git snapshot committed @ v{prev_version}")
    else:
        print("[version_bump] not a git repo — skipping snapshot")

    # Step 2: bump build counter.
    _write_build(new_build)

    # Step 3: regenerate src/version.h.
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    # v0.7.6: tag-based version. If RELEASE_TAG env is set (e.g. "v0.7.6"),
    # use it verbatim as FW_VERSION_STRING so the firmware reports a clean
    # release name. The build counter always stays in FW_VERSION_BUILD for
    # debug-tracking.
    #
    # v0.8.4: dev builds (no RELEASE_TAG) now report "<last-release>+<counter>"
    # (e.g. "0.8.4+1116") instead of "0.8.<counter>" (e.g. "0.8.1116"). The
    # counter moved behind the semver build-metadata '+', so the comparable
    # core stays a real release version. This fixes OTA: a dev build no longer
    # looks like a higher patch than the next release, so the device is offered
    # the update instead of refusing it as a "downgrade". See _last_release_core().
    release_tag = os.environ.get("RELEASE_TAG", "").strip()
    if release_tag.startswith("v"):
        release_tag = release_tag[1:]
    if release_tag:
        version_string = release_tag
    else:
        version_string = f"{_last_release_core(major, minor)}+{new_build}"
    header = f"""// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// AUTO-GENERATED by scripts/version_bump.py — do not edit by hand.
// Manual fields are version.txt (MAJOR.MINOR) and build_number.txt.
#ifndef BOSEFIX32_VERSION_H
#define BOSEFIX32_VERSION_H

// FW_NAME ist per build_flags uebersteuerbar (-D FW_NAME='"..."'). Das nutzt
// env:s3-max / env:c5-16mb-max: der Improv-Firmware-Name wird von esp-web-tools
// exakt gegen manifest.name gematcht, und die Max-Variante MUSS ein eigenes
// Update-Manifest bekommen (andere Partitionstabelle => andere Flash-Offsets).
// Gleicher Name = falsche Offsets = zerschossenes Geraet. Siehe gen_manifests.sh.
#ifndef FW_NAME
#define FW_NAME           "SixBack"
#endif
#define FW_VERSION_MAJOR  {major}
#define FW_VERSION_MINOR  {minor}
#define FW_VERSION_BUILD  {new_build}

#define FW_VERSION_STRING "{version_string}"
#define FW_BUILD_DATE     "{now}"

#endif // BOSEFIX32_VERSION_H
"""
    _atomic_write(HEADER_FILE, header)
    print(f"[version_bump] {new_version} @ {now}")

    # ----- Gzip data/index.html for LittleFS serving ------------------------
    # The hand-written UI HTML is ~110 KB. AsyncWebServer truncates large
    # responses under load on ESP32; serving a gzip-compressed copy gets us
    # ~75% smaller payload (~25 KB) which lands reliably. The backend's
    # handleRoot serves index.html.gz first if present.
    import gzip as _gzip
    data_dir = os.path.join(PROJECT_DIR, "data")
    # v0.7.4: HTML-Source lives in web-src/ (outside data/) so the uncompressed
    # 140 KB file does not get packed into the LittleFS image; the runtime
    # handler serves index.html.gz exclusively. Migration safety: if an older
    # checkout still has data/index.html, move it to web-src/.
    src_html = os.path.join(PROJECT_DIR, "web-src", "index.html")
    legacy_html = os.path.join(data_dir, "index.html")
    gz_html  = os.path.join(data_dir, "index.html.gz")
    if os.path.isfile(legacy_html):
        if not os.path.isfile(src_html):
            os.makedirs(os.path.dirname(src_html), exist_ok=True)
            os.replace(legacy_html, src_html)
            print(f"[migrate] moved legacy data/index.html -> web-src/index.html")
        else:
            os.remove(legacy_html)
            print(f"[migrate] removed stale data/index.html (web-src/ is source)")
    if os.path.isfile(src_html):
        src_mtime = os.path.getmtime(src_html)
        if (not os.path.isfile(gz_html) or
            os.path.getmtime(gz_html) < src_mtime):
            # NFS-safe Source-Read: validiert Inhalt + re-liest bei NUL-Korruption
            # (sonst gzippt der Build still eine kaputte web-src/index.html, deren
            # gz-Validate gegen das schon-korrupte 'raw' faelschlich besteht).
            raw = _read_text_source_nfs(src_html)
            # NFS-safe: build the gzip fully in memory, write atomically with
            # fsync, then read back + validate. A plain _gzip.open() write to the
            # NFS-backed data/ dir got flushed lazily, so mklittlefs occasionally
            # packed a NUL-prefixed (corrupt) index.html.gz into the LittleFS
            # image and the device served an undecodable page. Fail loud (after
            # retries) rather than ship a broken UI.
            gz_bytes = _gzip.compress(raw, compresslevel=9)
            for attempt in range(3):
                _atomic_write_bytes(gz_html, gz_bytes)
                try:
                    with open(gz_html, "rb") as fc:
                        check = fc.read()
                    if check[:2] != b"\x1f\x8b" or _gzip.decompress(check) != raw:
                        raise ValueError("gz read-back mismatch (NFS write-back?)")
                    break
                except Exception as e:
                    print(f"[gzip-ui] post-write validation failed "
                          f"(attempt {attempt+1}/3): {e}")
                    time.sleep(0.3)
            else:
                raise RuntimeError(
                    f"[gzip-ui] {gz_html} stayed corrupt after 3 atomic writes — "
                    f"aborting build to avoid flashing a broken UI.")
            print(f"[gzip-ui] {len(raw)} -> {len(gz_bytes)} bytes "
                  f"({100*len(gz_bytes)/max(1,len(raw)):.1f}%)")


main()
