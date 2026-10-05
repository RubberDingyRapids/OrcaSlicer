# Building this fork on Windows without a full rebuild

This branch is upstream OrcaSlicer commit `63d5fa23` (2.5.0-dev, 5 Oct 2026)
plus the **better rotate** camera patch (SolidWorks-style orbit about the point
under the cursor, with a pivot marker). The compiled dependencies and a
portable build are attached to the GitHub release so a change to the slicer
source only needs the 20-40 minute slicer build, not the 1.5 hour deps build.

## Paths matter

The compiled dependencies embed absolute paths (wxWidgets, libpng and the
pkg-config files reference `C:/dev/OrcaSlicer/deps/build/OrcaSlicer_dep`).
Use exactly these locations or the configure step will not find them:

| What | Where |
|---|---|
| This repo, checked out | `C:\dev\OrcaSlicer` |
| Compiled deps (from the release zip) | `C:\dev\OrcaSlicer\deps\build\OrcaSlicer_dep` |
| Slicer build output | `C:\dev\OrcaSlicer\build\OrcaSlicer` |
| Portable tools (no admin) | `C:\dev\tools` |

## One-time setup on a new machine

1. **Visual Studio 2022 Build Tools** with the "Desktop development with C++"
   workload (this is the only step needing admin):

       winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

2. **Portable tools** into `C:\dev\tools` (plain zips, no installer):
   - CMake: `cmake-4.4.4-windows-x86_64.zip` from github.com/Kitware/CMake/releases
   - Ninja: `ninja-win.zip` from github.com/ninja-build/ninja/releases, into `C:\dev\tools\ninja`
   - Strawberry Perl portable: `strawberry-perl-5.42.3.1-64bit-portable.zip`, into `C:\dev\tools\perl`

   `build-elliott.cmd` expects those folder names; adjust the `TOOLS` lines if
   you use other versions.

3. **Clone and fetch the deps**:

       git clone https://github.com/RubberDingyRapids/OrcaSlicer C:\dev\OrcaSlicer
       cd C:\dev\OrcaSlicer
       mkdir deps\build
       tar -xf <downloaded>\OrcaSlicer_dep_win64_msvc2022_63d5fa23.zip -C deps\build

   That leaves `deps\build\OrcaSlicer_dep\usr\local\...` in place.

4. **Host Python** for the bundled-CPython dependency. Only needed if you ever
   rebuild the deps; the slicer build does not need it. Any Python 3.10+ works;
   set `HOST_PYTHON` in `build-elliott.cmd` to its `python.exe`.

## Day-to-day

    C:\dev\OrcaSlicer\build-elliott.cmd slicer

Builds and installs into `C:\dev\OrcaSlicer\build\OrcaSlicer`. A small .cpp
change rebuilds in a couple of minutes; a header change in the GUI recompiles
much more (about 20 minutes). Close OrcaSlicer, then copy that folder over
your install.

To rebuild the deps from scratch (only after upstream changes `deps/`):

    C:\dev\OrcaSlicer\build-elliott.cmd deps

## Gotchas met on the first build

- `deps/python3` needs a host Python 3.10+; its finder only knows 3.10-3.13 via
  the `py` launcher and otherwise tries NuGet. `HOST_PYTHON` bypasses that.
- Windows Defender scans every object file written; excluding `C:\dev` (admin)
  speeds the deps build up noticeably.
- The MSVC generator builds the dependencies one after another. The Ninja
  generator (`-x`) builds independent ones concurrently and is faster for a
  deps rebuild, but the release deps were built with the MSVC generator.
