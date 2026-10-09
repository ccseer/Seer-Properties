# PDB test fixtures

Committed binary fixtures for the `pdb-property` tests. The repository's
`.gitignore` excludes `*.pdb` globally (that rule exists for build output),
so an explicit negation rule keeps these two files tracked.

- `full.pdb` (100 KB): a full PDB for a CRT-free `int main(void){return 0;}`
  program, produced with the VS 18 toolchain:
  `cl /nologo /Zi /c /O1 min.c` and
  `link /nologo /DEBUG /INCREMENTAL:NO /OUT:min.exe min.obj /NODEFAULTLIB
  /ENTRY:main /SUBSYSTEM:CONSOLE kernel32.lib /PDB:min.pdb`.
- `fastlink.pdb` (100 KB): a byte-level copy of `full.pdb` whose info-stream
  feature code `VC140` (20140508) was replaced with the `MINI` feature code
  (0x494E494D), which is what the linker records for `/DEBUG:FASTLINK`
  builds. The VS 18 toolchain refuses to emit real FASTLINK PDBs
  (`LNK4315: /DEBUG:FASTLINK is no longer supported`), so the fixture is
  produced by patching that single 4-byte feature code instead. The patch
  keeps every other byte identical, which is why the helper still reads real
  counts from it; the tests assert only the FASTLINK classification and the
  note row, never an absence of data.

Both fixtures report: format version `VC70` (20000404), machine `x64`
(DBI 0x8664), 2 modules, 1 public symbol, 1 global symbol, 6 section
contributions, 2 TPI type records. `fastlink.pdb` additionally reports
`UsesDebugFastLink = true` (and no IPI stream, because the IPI feature code
was the one replaced).

Regenerate them with any MSVC toolchain; only the two committed `.pdb` files
are tracked, never the `.exe`/`.obj` build products.
