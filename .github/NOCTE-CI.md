# NOCTE CI (M0)

How CI works in `NOCTE-dev/nocte-slicer`, what was changed from upstream
OrcaSlicer, and how to get a Windows build out of it.

Every fork-specific change to an upstream workflow is wrapped in
`# NOCTE-BEGIN ci` / `# NOCTE-END` so it survives (and is easy to re-apply after)
an upstream merge:

```bash
grep -rn "NOCTE-BEGIN ci" .github/
```

M0 goal: **CI is the only build machine.** Windows x64 builds on every push to
`main` and every pull request; everything else is opt-in or upstream-only.

---

## What runs where

| Workflow | Trigger on the fork | Runner | Notes |
| --- | --- | --- | --- |
| `build_all.yml` → `build_check_cache.yml` → `build_deps.yml` → `build_orca.yml` | push to `main`, PRs to `main`/`release/*` (path-filtered), manual | `windows-latest` (hosted) | Windows **x64 only**. Produces `nocte-slicer-windows-x64`. |
| `build_all.yml` Windows build-script test (`check_build_script`) | same | `windows-latest` | Runs `scripts/test_build_win.ps1`; gates the Windows build. |
| `build_all.yml` unit tests (Windows x64) | after the Windows build | `windows-latest` | `unit_tests.yml`, ctest + JUnit upload. No secrets. |
| `build_all.yml` Linux / macOS / Flatpak legs | **manual dispatch only** | hosted ubuntu / macos-14 | Still fully functional, just not run on push/PR. |
| `nocte-checks.yml` | push to `main`, all PRs, manual | `ubuntu-latest` | Touchpoint audit + Python byte-compile. Informational. |
| `check_locale.yml` | PRs touching `localization/**` | `ubuntu-latest` | Kept — `msgfmt` only, no secrets. |
| `check_profiles.yml` | PRs touching `resources/profiles/**`, `resources/printers/**`, `scripts/**` | `ubuntu-24.04` | Kept — no secrets. |
| `check_profiles_comment.yml` | after `check_profiles.yml` | `ubuntu-24.04` | Kept — uses only the default `GITHUB_TOKEN`. |
| `shellcheck.yml` | push/PR touching `**.sh`, nightly | `ubuntu-latest` | Kept — no secrets. |
| everything else (see below) | **disabled on the fork** | — | Guarded with `if: github.repository == 'OrcaSlicer/OrcaSlicer'`. |

Path filters: the Windows build only starts when the push/PR touches something
that can change the binary — `deps/**`, `deps_src/**`, `src/**`, any
`CMakeLists.txt`, `version.inc`, `resources/**`, `localization/**`, `tests/**`,
`build_win.bat`, the build scripts, or `.github/workflows/build_*.yml`.
A change to `tools/**` or docs alone will **not** start a build; use
*Run workflow* if you want one anyway. (`resources/**` and `localization/**`
were added to the PR filter for the fork: the shipped binary embeds a preset
cache generated from `resources/profiles`, so profile PRs must rebuild.)

---

## Getting the Windows build

Artifacts of a finished run, on `github.com/NOCTE-dev/nocte-slicer`:

1. **Actions** tab → workflow **Build all** → click the run (top of the list is
   the newest).
2. Scroll to the bottom of the run summary page → **Artifacts**.
3. Download:
   - **`nocte-slicer-windows-x64`** — the portable build. GitHub serves it as
     `nocte-slicer-windows-x64.zip`; unzip it anywhere and run
     `OrcaSlicer/orca-slicer.exe`. No installation, no admin rights.
   - `nocte-slicer-windows-x64-installer` — the NSIS installer (`.exe`) for the
     same build, if you prefer a normal install.
   - `OrcaSlicer_profile_validator_Windows_<ver>` — CLI profile validator.
   - `test-results-<sha>-tests-windows-x64` — JUnit XML from the unit tests.

Inside the container the tree is the installed layout produced by
`build_win.bat`, i.e. `build/OrcaSlicer/` in the runner workspace:
`orca-slicer.exe` plus `resources/`.

Artifacts expire with the repository's retention setting (90 days by default;
test artifacts are pinned to 5 days).

### Download from the command line

```bash
gh run list --workflow build_all.yml --branch main --limit 5
gh run download <run-id> -n nocte-slicer-windows-x64 -D ./win-build
```

---

## Expected duration

| Situation | Windows leg |
| --- | --- |
| First run ever / after any change under `deps/` (deps cache miss) | **1–3 h** (dependencies are compiled from source, then the slicer) |
| Normal run, deps cache hit, warm compiler cache | **30–60 min** |
| Normal run, deps cache hit, cold compiler cache (first run after a cache eviction) | ~60–90 min |

Why: `build_deps.yml` builds Boost, wxWidgets, OpenCV, CGAL, OCCT and friends.
That output is cached, so it is paid once per change to `deps/`.

### The deps cache — do not break it

- `build_check_cache.yml` computes
  `cache-key = windows-x64-clang-cache-orcaslicer_deps-build-<hashFiles('deps/**')>`
  and does a `lookup-only` probe.
- On a miss, `build_deps.yml` runs `build_win.bat -d` and **saves** that key.
- On a hit, `build_deps` is skipped entirely and `build_orca.yml` restores the
  same key with `fail-on-cache-miss: true`.
- Therefore: **touching anything under `deps/` invalidates the cache and costs
  1–3 h on the next run.** Batch dependency changes.
- A separate `ccache` entry (`ccache-Windows-x64-clang-vc<toolset>-…`, max 3 GB)
  caches object files. Pushes save it; PRs only restore it — so the first PR
  after a long gap is slower.
- GitHub gives a repository 10 GB of Actions cache and evicts least-recently-used
  entries. The fork only builds one leg, so deps + ccache fit; if you later
  re-enable Linux/macOS on push, expect eviction thrash and longer builds.

**Warming the cache deliberately:** Actions → *Build all* → *Run workflow* →
tick **“Only build dependencies (bypasses caching)”**. That runs only
`build_deps` (1–3 h) and populates the cache; the unit-test and slice-check legs
are skipped for such a run.

---

## Building macOS / Linux / Flatpak on demand

Actions → **Build all** → **Run workflow** → branch `main` → **Run workflow**.

A `workflow_dispatch` run builds *everything*: Windows x64, Linux x86_64 +
aarch64, macOS arm64 + x86_64 (+ the universal bundle) and both Flatpak arches,
each with its own deps cache and its own 1–3 h first run. There is no per-OS
checkbox — that is upstream's matrix and the fork has not re-cut it. To build
just one of them, either let it run (unused legs fail independently,
`fail-fast: false`) or temporarily flip that job's NOCTE guard in
`build_all.yml`.

macOS artifacts are **unsigned and un-notarized** on the fork (see below), so
macOS will refuse to open the app until you clear the quarantine attribute:
`xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app`.

---

## `nocte-checks.yml`

Fork-only workflow. On every push to `main` and every PR, on `ubuntu-latest`,
Python 3.12:

1. Adds the `upstream` remote (`https://github.com/OrcaSlicer/OrcaSlicer.git`),
   fetches `upstream/main` (blobless) and resolves the comparison base as
   `git merge-base HEAD upstream/main`, falling back to the pinned base commit `6b0e190e64` (tag `nocte-base-2026-09-16`) `main`
   is based on when there is no shared history.
2. Runs `python tools/nocte/check_touchpoints.py --base <that commit>`.
3. Runs `python -m compileall tools/bbl-compat tools/nocte`.

Steps 2 and 3 emit a `::warning::` and pass if the paths are not in the tree yet
(the tooling is landing in parallel); once the files exist, a non-zero exit
fails the job.

This job is **not** a required status check and must not be added to branch
protection while M0 is in flight — it reports, it does not gate.

---

## Disabled on the fork, and why

All of these keep working unchanged upstream; each carries a
`# NOCTE-BEGIN ci` guard `if: github.repository == 'OrcaSlicer/OrcaSlicer'`.

| Workflow | Why it cannot work here |
| --- | --- |
| `publish_release.yml` | OrcaSlicer's release pipeline; gated on the `RELEASE_PUBLISHER` repo variable and uploads to OrcaSlicer draft releases. |
| `winget_updater.yml` | Needs the `WINGET_TOKEN` secret and publishes the `SoftFever.OrcaSlicer` WinGet manifest. Would fire (and fail) on any fork release. |
| `update-translation.yml` | Regenerates the catalogs and pushes a commit to the default branch. |
| `parity_nightly.yml` | Pulls the latest successful upstream `build_all` AppImage and drives `OrcaSlicer/orca-test-repo`. Guarded for dispatch too, not just the cron. |
| `doxygen-docs.yml` | *Already* upstream-guarded (`github.repository == 'OrcaSlicer/OrcaSlicer' && refs/heads/main`); needs the `R2_*` Cloudflare secrets. Left untouched. |
| `pr-merge-bot.yml` | *Already* upstream-guarded; needs the `merge-delegation` environment and the `FOLDER_MERGERS` variable. Left untouched. |
| `pr-label-bot.yml` | Upstream triage labels/process (all three jobs guarded). |
| `assign.yml` | Assigns issues to OrcaSlicer maintainers on a nightly cron. |
| `dedupe-issues.yml` | Needs `CLAUDE_CODE_OAUTH_TOKEN` and `STATSIG_API_KEY`. |
| `auto-close-duplicates.yml` | Needs `STATSIG_API_KEY`; acts on OrcaSlicer's issue tracker. |
| `backfill-duplicate-comments.yml` | Same duplicate-detection bot, manual backfill. |
| Nightly cron in `build_all.yml` | Removed: no nightly release to publish to, and it would burn hosted minutes every night. |
| Windows **arm64** leg | Needs the `windows-11-vs2026-arm` runner label, which a plain fork does not have. |
| Windows **PDB** packing/upload | `-mx9` LZMA2 over ~1 GB of PDBs adds ~10 min per run; no symbol server to feed. Re-enable by dropping the repository guard on the two `Pack PDB` / `Upload artifacts Win PDB` steps in `build_orca.yml`. |
| Windows **MSIX** package | Microsoft Store packaging keyed to OrcaSlicer's publisher identity (`ORCA_MSIX_*` variables). |
| The nightly `deploy-nightly` upload steps | Upstream-guarded already: they push to OrcaSlicer release 137995723. |
| macOS signing / notarization | Upstream-guarded already: `BUILD_CERTIFICATE_BASE64`, `P12_PASSWORD`, `KEYCHAIN_PASSWORD`, `MACOS_CERTIFICATE_ID`, `APPLE_DEV_ACCOUNT`, `TEAM_ID`, `APP_PWD`. A dispatch on the fork takes the "Create DMG without notary" path. |

Other upstream-only things that are *harmless* here and were left alone:

- `ORCA_UPDATER_SIG_KEY` (`secrets.ORCA_UPDATER_SIG_KEY`) is empty on the fork.
  `src/slic3r/CMakeLists.txt` treats an empty key as "no updater signing key"
  (`ORCA_UPDATER_SIG_KEY_AVAILABLE 0`) — the build succeeds, the built slicer
  just cannot verify signed updates.
- `vars.SELF_HOSTED` is unset on the fork, so every `vars.SELF_HOSTED &&` branch
  falls through to the GitHub-hosted runner. Do **not** set that variable.
- `secrets: inherit` on the reusable-workflow calls is fine — it inherits an
  empty set.
- `.github/dependabot.yml` is upstream's. GitHub disables Dependabot on forks by
  default; leave it that way unless you want action-version PRs.

---

## Gotchas

- **PRs from a fork of this fork** get a read-only `GITHUB_TOKEN`: they can
  restore caches but not save them, and the "Publish Test Results" step cannot
  write its check (it is `continue-on-error`, so it will not fail the run).
- A Windows build fills a large part of the hosted runner's disk (deps prefix +
  build tree + ccache). If a run dies with "no space left on device" or a linker
  out-of-memory error rather than a compile error, re-run it before debugging —
  those failures are not deterministic.
- After merging upstream, re-check every `NOCTE-BEGIN ci` block: upstream
  rewrites `build_all.yml`/`build_orca.yml` often.
- Validate any workflow edit before pushing:
  ```bash
  python -c "import yaml,sys; [yaml.safe_load(open(f, encoding='utf-8')) for f in sys.argv[1:]]" .github/workflows/*.yml
  ```
