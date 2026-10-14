"""Streaming writes: a large write is uploaded chunk by chunk while it is being
written, so it needs ~one chunk of memory instead of the whole file, and the
file is published only by the commit on close.

Memory is proven twice over: by the extension's own high-water mark
(sharepoint_stats() 'upload_buffer_peak') and by the duckdb process's peak RSS
as the kernel reports it (wait4), against a file several times larger.
"""

from __future__ import annotations

import os
import subprocess
import threading
import time

import pytest

from conftest import scrub

pytestmark = pytest.mark.live

MiB = 1024 * 1024
CHUNK = 10 * MiB  # the default sharepoint_upload_chunk_bytes


def _run_measured(duckdb_cli, env, statements: str) -> tuple[int, str, int]:
    """Run SQL in a fresh duckdb process; return (exit code, output, peak RSS
    in bytes) -- the RSS of THAT process, from wait4, not of any other child."""
    proc = subprocess.Popen([str(duckdb_cli), "-noheader", "-list"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    assert proc.stdin and proc.stdout
    proc.stdin.write(statements)
    proc.stdin.close()
    out = proc.stdout.read()
    _, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = os.waitstatus_to_exitcode(status)
    return proc.returncode, out, usage.ru_maxrss * 1024  # Linux reports KiB


@pytest.mark.slow
def test_large_copy_streams_in_bounded_memory(graph, scratch, duckdb_cli, isolated_home, secret_sql):
    # ~400 MB of CSV from a generator that itself needs almost no memory.
    rows = 3_600_000
    uri = f"sharepoint://{scratch}/big.csv"
    code, out, rss = _run_measured(duckdb_cli, isolated_home, f"""{secret_sql}
        CALL sharepoint_reset_stats();
        COPY (SELECT i, repeat('x', 100) AS pad FROM range({rows}) t(i)) TO '{uri}' (HEADER);
        SELECT metric || '=' || value FROM sharepoint_stats()
         WHERE metric IN ('upload_session', 'upload_chunk', 'upload_commit', 'upload_put', 'upload_buffer_peak');
        SELECT 'size=' || file_size('{uri}');
    """)
    assert code == 0, scrub(out)
    stats = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    size = int(stats["size"])
    assert size > 350 * MiB
    assert stats["upload_session"] == "1"
    assert stats["upload_commit"] == "1"
    assert stats["upload_put"] == "0"
    assert int(stats["upload_chunk"]) == -(-size // CHUNK)
    # The extension never held more than one chunk...
    assert int(stats["upload_buffer_peak"]) == CHUNK
    # ...and the process as a whole stayed far below the file size. Buffering
    # the file (the old behaviour) cannot finish under `size` bytes of RSS.
    print(f"peak RSS {rss / MiB:.0f} MiB for a {size / MiB:.0f} MiB upload")
    assert rss < size / 2, f"peak RSS {rss / MiB:.0f} MiB for a {size / MiB:.0f} MiB file"

    # And Graph holds exactly those bytes.
    assert graph.stat(f"{scratch}/big.csv")["size"] == size


def test_streamed_bytes_round_trip_exactly(scratch, sql, secret_sql, tmp_path):
    # Several chunks at the 320-KiB minimum, compared byte for byte with the
    # same COPY to local disk.
    uri = f"sharepoint://{scratch}/s.csv"
    local = tmp_path / "s.csv"
    out = sql(f"""{secret_sql}
        SET sharepoint_upload_chunk_bytes = 327680;
        CALL sharepoint_reset_stats();
        COPY (SELECT i, md5(i::VARCHAR) AS h FROM range(60000) t(i)) TO '{uri}' (HEADER);
        COPY (SELECT i, md5(i::VARCHAR) AS h FROM range(60000) t(i)) TO '{local}' (HEADER);
        SELECT (SELECT value FROM sharepoint_stats() WHERE metric = 'upload_chunk') || '|' ||
               (SELECT md5(content) FROM read_blob('{uri}')) || '|' ||
               (SELECT md5(content) FROM read_blob('{local}'));""")
    chunks, remote_md5, local_md5 = out.splitlines()[-1].split("|")
    assert int(chunks) == -(-local.stat().st_size // 327680) and int(chunks) > 3
    assert remote_md5 == local_md5


def test_failure_mid_stream_publishes_nothing(graph, scratch, duckdb_cli, isolated_home, secret_sql):
    """The destination's folder vanishes while the upload is streaming. The COPY
    must FAIL (not report success for a file that is not there), and nothing may
    be left at the destination."""
    graph.mkdir(f"{scratch}/doomed")
    uri = f"sharepoint://{scratch}/doomed/x.csv"
    proc = subprocess.Popen([str(duckdb_cli), "-noheader", "-list"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=isolated_home)
    assert proc.stdin
    # Big enough to still be streaming when the folder goes (~200 MB in 320 KiB
    # chunks), written straight to the target name so the placeholder Graph
    # shows for an open session is visible to the poll below.
    proc.stdin.write(f"""{secret_sql}
        SET sharepoint_upload_chunk_bytes = 327680;
        COPY (SELECT i, repeat('x', 100) AS pad FROM range(2000000) t(i)) TO '{uri}' (HEADER, USE_TMP_FILE false);
        SELECT 'copy-returned';
    """)
    proc.stdin.close()
    output: list[str] = []
    reader = threading.Thread(target=lambda: output.append(proc.stdout.read()), daemon=True)
    reader.start()

    deadline = time.time() + 120
    # SharePoint shows a 0-byte placeholder item (stat finds it; a folder
    # listing does not) as soon as an upload session is open on the path.
    while time.time() < deadline and graph.stat(f"{scratch}/doomed/x.csv") is None:
        time.sleep(0.5)
    assert graph.stat(f"{scratch}/doomed/x.csv") is not None, "the upload session never opened"
    assert graph.delete(f"{scratch}/doomed", permanent=True)

    proc.wait(timeout=900)
    reader.join(timeout=30)
    text = scrub("".join(output))
    assert proc.returncode != 0 or "copy-returned" not in text, f"COPY reported success:\n{text}"
    assert "Error" in text, text
    print("mid-stream failure surfaced as:", [l for l in text.splitlines() if "Error" in l][:1])
    assert graph.stat(f"{scratch}/doomed/x.csv") is None
    assert graph.stat(f"{scratch}/doomed") is None
