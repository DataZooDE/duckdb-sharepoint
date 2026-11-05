"""read_docx / read_docx_table (#103) through sharepoint://: the fixture's
paragraphs (with heading levels) and its table, and a glob over two files.
COPY ... (FORMAT docx, TEMPLATE ...) (#104): a template in SharePoint filled
into a new file there, and again over it (COPY's temporary file and move)."""

from __future__ import annotations

import pytest

from conftest import REPO_ROOT

pytestmark = pytest.mark.live

FIXTURE = (REPO_ROOT / "test" / "data" / "docx" / "fixture.docx").read_bytes()
TEMPLATE = (REPO_ROOT / "test" / "data" / "docx" / "template" / "quote.docx").read_bytes()


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


def test_docx_template_filled_into_sharepoint(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/templates/quote.docx", TEMPLATE)
    graph.mkdir(f"{scratch}/out")
    out = f"sharepoint://{scratch}/out/quote.docx"
    tmpl = f"sharepoint://{scratch}/templates/quote.docx"
    lines = sql(f"""{secret_sql}
        COPY (SELECT * FROM (VALUES ('North', 10), ('South', 7)) t(region, units))
        TO '{out}' (FORMAT docx, TEMPLATE '{tmpl}',
                    VARIABLES {{'customer': 'ACME', 'valid_until': DATE '2026-12-31', 'quote_no': 'Q-1'}});
        SELECT string_agg(text, '|' ORDER BY idx) FROM read_docx('{out}');
        SELECT string_agg(Region || '=' || Units, ',') FROM read_docx_table('{out}');
        COPY (SELECT 'West' AS region, 3 AS units)
        TO '{out}' (FORMAT docx, TEMPLATE '{tmpl}',
                    VARIABLES {{'customer': 'Initech', 'valid_until': 'soon', 'quote_no': 'Q-2'}});
        SELECT string_agg(text, '|' ORDER BY idx) FROM read_docx('{out}');
        SELECT string_agg(Region || '=' || Units, ',') FROM read_docx_table('{out}');
        SELECT string_agg(text, '|' ORDER BY idx) FROM read_docx('{tmpl}');
    """).splitlines()
    assert lines[-5:] == [
        "Quote for ACME|Valid until 2026-12-31|Thank you.",
        "North=10,South=7",
        "Quote for Initech|Valid until soon|Thank you.",
        "West=3",
        # The template is untouched. (Not by size: SharePoint rewrites an
        # uploaded .docx to embed its document properties, hld §7a.)
        "Quote for {{customer}}|Valid until {{ valid_until }}|Thank you.",
    ], lines
    # The overwrite left no temporary file behind.
    assert [c["name"] for c in graph.children(f"{scratch}/out")] == ["quote.docx"]
