# Sources of the radio region tables

The tables in `flipper/rootfs/usr/share/region/` (spec section Q) are checked
against the texts in this directory (T32b, issue #382). One file per source:
where it was retrieved, when, which version, the exact section or page, and the
limit quoted word for word. The regulations are not copied whole: each file
quotes only the limits the tables use and says where they are.

## How a row points here

A table row's `ref` field (the last column) says whether the row is verified:

| Row | `ref` starts with | Meaning |
|---|---|---|
| active (no `#?`) | `[id]` | verified: `docs/regulation/<id>.md` quotes the limit; the rest of the `ref` names the row or section |
| `#?...` | `unverified` | not verified, with the reason; never used for transmit (Q.2) |

So "verified" needs no new field in the Q.2 format and no change to the RQ.2
record: a `#?` row is never compiled into the record (`mdm_rg_compile`), and
the kernel can only transmit on rows in the record. `make modemtest` checks for
every shipped table that each active row names an existing file here, that each
`#?` row says `unverified`, that the record holds exactly the verified rows,
and that a sweep over every unverified row's band never picks it.

| `id` | Source | File |
|---|---|---|
| `eu-2025-105` | Decision 2006/771/EC, annex as replaced by Implementing Decision (EU) 2025/105 | [eu-2025-105.md](eu-2025-105.md) |
| `erc-70-03` | ERC/REC 70-03, edition of October 2026: Annex 1, Appendix 1, Appendix 2 | [erc-70-03.md](erc-70-03.md) |
| `cfr-47-15` | 47 CFR part 15: 15.205, 15.231, 15.247, 15.249 (eCFR) | [cfr-47-15.md](cfr-47-15.md) |
| `rss-210` | ISED RSS-210 Issue 11, with RSS-Gen Issue 6 section 7.5 | [rss-210.md](rss-210.md) |

## Not read (rows that depend on them stay `#?`)

- **ISED RSS-247** (now Issue 4, 2025): the ISED page could not be retrieved
  (404 at the expected address, the Issue 3 PDF timed out). The `CA` `fhss`
  row is `#?` until it is read. (The `fhss` rows are never chosen by the
  row choice anyway; the hop table is checked at arm time, R19.11.)
- **National texts** of AKEP (AL), RAK (BA), AEK (MK), Nkom (NO), Ofcom IR 2030
  (GB) and of the EU member states for 870–876 / 915–921 MHz (Decision (EU)
  2018/1538, as amended by (EU) 2022/172 and (EU) 2025/650). The h2/h3 rows
  stay `#?` everywhere (Q.5). For AL, BA, MK, NO and GB the active rows rest
  on ERC/REC 70-03 Annex 1 plus that country's Appendix 1 status "Y"; Q.6 lists
  70-03 as the source for these countries.

## Repeating the check

1. Fetch each source again (the URLs are in the files). EUR-Lex refuses
   scripted downloads; the Publications Office serves the same act by CELEX
   number with `Accept: application/xhtml+xml`. CEPT's document database
   lists the latest 70-03 edition under document 845.
2. Compare every quoted limit and every Appendix 1 status with the new text.
   If a number changed, correct the row and its file here; if a source cannot
   be read, turn the row into `#?` with `unverified: <reason>`.
3. Update `Checked:` in the table headers and run `make modemtest`.
