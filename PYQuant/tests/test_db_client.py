"""DbClient 검사 — 묶음 적재의 COPY 텍스트 변환(_copy_text)과 WSL 직결 주소 조회(wsl_direct_host).

COPY 는 한 글자만 어긋나도 묶음이 통째로 빠지거나 칸이 밀린다 — NULL·구분자·줄바꿈·역슬래시·bool·시각을
글자로 바꾸는 규칙을 고정해 둔다. DB 가 떠 있으면(TSDB_PASSWORD) 임시 표에 실제로 넣고 되읽어 본다.
WSL 주소 조회는 wsl 명령을 가짜로 바꿔, 못 물었을 때 None(= localhost 로 붙음)이 되는지를 본다.
"""
import os
import subprocess
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import client as db_client
from db.client import _copy_text, wsl_direct_host


def _completed(stdout: str) -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(args=[], returncode=0, stdout=stdout, stderr="")


class TestWslDirectHost(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ, {"TSDB_WSL_DIRECT": "1"})
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = mock.patch.object(db_client.os, "name", "nt")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_reads_eth0_address(self):
        line = "2: eth0    inet 172.28.101.77/20 brd 172.28.111.255 scope global eth0\\       valid_lft forever\n"

        with mock.patch.object(db_client.subprocess, "run", return_value=_completed(line)):
            self.assertEqual(wsl_direct_host("localhost"), "172.28.101.77")

    def test_other_host_is_left_alone(self):
        with mock.patch.object(db_client.subprocess, "run") as run:
            self.assertIsNone(wsl_direct_host("timescaledb"))
            run.assert_not_called()

    def test_switch_off(self):
        with mock.patch.dict(os.environ, {"TSDB_WSL_DIRECT": "0"}), \
             mock.patch.object(db_client.subprocess, "run") as run:
            self.assertIsNone(wsl_direct_host("localhost"))
            run.assert_not_called()

    def test_lookup_failure_falls_back(self):
        with mock.patch.object(db_client.subprocess, "run", side_effect=OSError("wsl 없음")):
            self.assertIsNone(wsl_direct_host("localhost"))

        with mock.patch.object(db_client.subprocess, "run", return_value=_completed("")):
            self.assertIsNone(wsl_direct_host("127.0.0.1"))


class TestCopyText(unittest.TestCase):
    def test_values_become_copy_text(self):
        stamp = datetime(2026, 9, 26, 1, 2, 3, 456000, tzinfo=timezone.utc)
        text = _copy_text([(stamp, "005930", 71200.0, None, True, "a\tb\nc\\d")]).read()
        self.assertEqual(text, "2026-09-26T01:02:03.456000+00:00\t005930\t71200.0\t\\N\tTrue\ta\\tb\\nc\\\\d\n")

    def test_one_line_per_row(self):
        text = _copy_text([(1, "x"), (2, "y")]).read()
        self.assertEqual(text, "1\tx\n2\ty\n")


@unittest.skipUnless(os.getenv("TSDB_PASSWORD"), "TSDB_PASSWORD 없음 — DB 왕복은 건너뜀")
class TestCopyRoundTrip(unittest.TestCase):
    def test_round_trip(self):
        from db.client import DbClient

        client = DbClient(retries=1)
        stamp = datetime(2026, 9, 26, 1, 2, 3, 456000, tzinfo=timezone.utc)
        rows = [(stamp, "005930", 71200.5, None, True, "탭\t줄\n역\\"), (stamp, "000660", 1.0, 7, False, None)]

        try:
            with client._cursor() as cursor:
                cursor.execute("CREATE TEMP TABLE copy_check (ts timestamptz, ticker text, price double precision,"
                               " volume integer, ok boolean, note text)")

            self.assertEqual(client._insert_batch("copy_check", "ts,ticker,price,volume,ok,note", rows), 2)

            with client._cursor() as cursor:
                cursor.execute("SELECT ts, ticker, price, volume, ok, note FROM copy_check ORDER BY ticker DESC")
                self.assertEqual(cursor.fetchall(), rows)
        finally:
            client.close()


if __name__ == "__main__":
    unittest.main()
