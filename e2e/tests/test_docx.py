"""read_docx / read_docx_table (#103) through sharepoint://: the fixture's
paragraphs (with heading levels) and its table, and a glob over two files."""

from __future__ import annotations

import pytest

from conftest import REPO_ROOT

pytestmark = pytest.mark.live

FIXTURE = (REPO_ROOT / "test" / "data" / "docx" / "fixture.docx").read_bytes()


def test_docx_paragraphs_and_tables(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/a.docx", FIXTURE)
    graph.upload(f"{scratch}/b.docx", FIXTURE)
    out = sql(f"""{secret_sql}
        SELECT string_agg(coalesce(heading_level::VARCHAR, '-') || ':' || text, '|' ORDER BY idx)
        FROM read_docx('sharepoint://{scratch}/a.docx');
        SELECT sum(Units::INTEGER) FROM read_docx_table('sharepoint://{scratch}/a.docx', 1);
        SELECT count(DISTINCT file), count(*) FILTER (WHERE text ILIKE '%notice period%')
        FROM read_docx('sharepoint://{scratch}/*.docx');
    """).splitlines()
    assert out[-3:] == ["1:Service contract|-:Notice period: 3 months|2:Terms|-:Fees & payment", "17", "2|2"], out
