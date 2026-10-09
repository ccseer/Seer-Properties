# Vendored raw_pdb

This directory vendors [raw_pdb](https://github.com/MolecularMatters/raw_pdb),
a C++11 library that reads Microsoft Program DataBase (PDB) files directly
from memory-mapped data. It is the parsing engine of the `pdb-property`
package.

- Source revision: main @ `43cc59b` (merge of PR #103, 2026-06-24). The
  project publishes no release tags, so the commit is pinned here.
- License: BSD 2-Clause (`LICENSE`, Copyright 2011-2022 Molecular Matters GmbH).
- Contents: the library sources only (`src/PDB*.h`, `src/PDB*.cpp`,
  `src/Foundation/*.h`). The upstream `Examples/` directory and the upstream
  `CMakeLists.txt` are not vendored; each package builds its own static-lib
  target from an explicit source list.
- Layout: everything lives under `src/` so a single include directory
  (`.../third_party/rawpdb/src`) exposes `PDB.h`, `PDB_RawFile.h` and friends.

Caveats observed while integrating:

- The library validates only the MSF superblock (`PDB::ValidateFile`). Beyond
  that it trusts the stream data and reports inconsistencies through
  `PDB_ASSERT`, which compiles to a no-op in release builds. Callers that
  parse untrusted files must check stream counts and sizes before touching
  higher-level streams and should run the query behind a structured-exception
  guard (the `pdb-property` helper does both).
- Constructing a stream (`Create*Stream` / `InfoStream`) copies ("coalesces")
  that whole stream into one contiguous buffer. The symbol record stream of a
  multi-gigabyte PDB is hundreds of megabytes; the `pdb-property` helper only
  creates the small header/count streams and never materializes it.
