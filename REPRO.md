# PS-11175 — reproduce the page-tracking truncation crash (B → A)

This branch reproduces, on **clean 8.0** with **one debug knob** (no seeding,
no manual file deletion), the page-archive 2-byte truncation bug and both of
its customer-visible failures, in a single datadir lineage:

- **FACE B** — `mysqlbackup_page_track_get_changed_pages()` makes the server
  **exit** (`MY-012646`, ENOENT on a purged `ib_page_*`).
- **FACE A** — after that, the server **will not restart** (`MY-012642`,
  32-byte read past EOF), and **every** restart attempt fails the same way
  (truly unrestartable).

## Root cause (one line)
`Arch_Block::add_reset()` persists `reset_pos.m_block_num` via
`mach_write_to_2` — once the archiver's `m_block_num` exceeds 65535 the saved
reset position is truncated to 16 bits, so recovery / `get_changed_pages`
dereference a wrong/absent archive file.

## What's on this branch
```
PS-11175 [8.0]: page-id bombard knob to reproduce the truncation crash (B->A->A)
PS-11247 [8.0]: fix Arch_File_Ctx m_count off-by-N (spurious files, bad purge recovery)
PS-11221 [8.0]: fix page-tracking archiver wait hang
```
- **PS-11221** (fix) — without it, page tracking just *hangs* under load (the
  32-block ring fills and a thread spins on a stale block state); needed so the
  repro can drive `m_block_num` past 65535 instead of hanging.
- **PS-11247** (fix) — without it, the *second* restart after the crash spawns
  a spurious file and silently purges the tracking group; with it, the crash
  stays fatal on every restart (true "unrestartable").
- **PS-11175** (repro knob + scripts) — `innodb_arch_page_bombard` caps page-id
  entries per archive block so `m_block_num` crosses 65535 in minutes under an
  ordinary workload, with a faithful `ib_page_0..N` layout. `full_baa.sh`
  drives the whole sequence.

> The two PS-11221 / PS-11247 commits are the actual fixes. The PS-11175 commit
> is the reproduction tooling (the `innodb_arch_page_bombard` knob is debug/test
> only — do not ship it).

## Prerequisites
- Linux, ~6 GB free disk for the scratch datadir, ~8 GB RAM.
- `sysbench` 1.0.20+ on PATH.
- A **release** (RelWithDebInfo) build of this branch including the
  `component_mysqlbackup` component.

## 1. Get the branch
```sh
git clone https://github.com/satya-bodapati/percona-server.git
cd percona-server
git checkout ps_11175_simulate_page_id
```

## 2. Build (release)
```sh
mkdir -p bld_rel && cd bld_rel
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDOWNLOAD_BOOST=1 -DWITH_BOOST=../boost \
         -DWITH_PACKAGE_FLAGS=OFF
make -j"$(nproc)" mysqld mysql component_mysqlbackup
cd ..
```
Notes:
- `-DWITH_PACKAGE_FLAGS=OFF` avoids a `_FORTIFY_SOURCE` redefinition error on
  recent GCC (e.g. GCC 13 / Ubuntu 24.04). Drop it if your toolchain doesn't hit that.
- The `innodb_arch_page_bombard` knob is **not** under `UNIV_DEBUG`, so a
  release build is enough — no debug build required.
- Verify the knob exists:
  `bld_rel/runtime_output_directory/mysqld --no-defaults --verbose --help | grep arch-page-bombard`

## 3. Run the reproduction
`full_baa.sh` is self-contained (it inits a fresh datadir, starts mysqld with
the right settings, runs the workload, and drives both faces). Point it at your
build dir and a scratch workdir:
```sh
BLD="$PWD/bld_rel" WORKDIR=/var/tmp/ps11175_baa ./full_baa.sh
```
Runtime ≈ 8–12 min (most of it is bombarding `m_block_num` past 65535).

## 4. Expected output (the proof)
```
>>> L_MID lsn=...           (low purge-anchor reset, file ~4)
>>> E_B   lsn=... [TRUNCATED] (deref -> ib_page_0)   (the poisoned reset)
restart #1 OK
>>> E_A in ib_page_35 size=... (past-EOF reset in a fresh tail)
purge_up_to(L_MID) -> ...    (real purge; removes ib_page_0..3)
client: ERROR 2013 (HY000) ... Lost connection            <-- FACE B: SELECT killed the server
FACE B: server DEAD (exit):
   [ERROR] [MY-012646] File #ib_archive/.../ib_page_0: 'open' returned OS error 71. Cannot continue operation
restart #1: did NOT come up
   [ERROR] [MY-012642] Tried to read 32 bytes at offset ... ib_page_35     <-- FACE A
restart #2: did NOT come up   (MY-012642)
restart #3: did NOT come up   (MY-012642)
=== SUMMARY ===
  faceA restart1: MY-012642=1 up=0 purge=0
  faceA restart2: MY-012642=1 up=0 purge=0
  faceA restart3: MY-012642=1 up=0 purge=0
```
**Success criteria:** `Face B` crashes the server (ERROR 2013 / MY-012646), and
all three FACE-A restarts show `MY-012642=1 up=0 purge=0` (fatal every time, no
silent purge). Logs/`*.out` are kept under `$WORKDIR`.

## Other scripts (optional)
- `natural_faceb.sh` — FACE B only (purge + `get_changed_pages`), faster.
- `obs_simulate.sh` — observation run: bombard across 65535 and dump the
  `save_reset_point` real-vs-truncated diagnostics + archive layout.
- `run_simulate_page_id.sh` — FACE A only (restart → `MY-012642`).

## How to confirm it's the bug, not the harness
Build the same branch **without** the fix commits (or `git revert` PS-11221) and
the bombard phase **hangs** instead — that's PS-11221. Keep PS-11221 but revert
PS-11247 and the *second* FACE-A restart comes up with the archive purged
(`purge=1`) instead of staying fatal — that's PS-11247.
