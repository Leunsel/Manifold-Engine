# Testing

This is the testing half of [AGENTS.md](../AGENTS.md): how a bug is shown, how to build and run the tests, and what must hold before a commit. The suites themselves are listed in [DEVELOPMENT.md](DEVELOPMENT.md#tests).

## Before any test run

- **No game may be running.** The tests start debuggers, inject into test targets and load the CPU. Look at `tasklist` before every run. If a game runs (anything from a game library, any anti-cheat game), do not start the tests: say what is ready to run and wait.
- Tests run only against the programs of your own build: `Manifold-Engine.Tests.exe`, `TestTarget.exe` and `TestTarget32.exe`. Nothing else is attached to or written to.
- `--compat` runs of a table always get `--home=<a new folder in your Build directory>`. The headless harness answers every confirmation with No and only prints shell commands; keep it that way.

## Build

From a worktree (each worktree builds into its own `Build` folder):

```bash
msbuild Manifold-Engine.sln /p:Configuration=Release /p:Platform=x64 /m:2 /p:CL_MPCount=3
```

- From Git Bash, set `MSYS2_ARG_CONV_EXCL='*'` on the msbuild command only. Exported in the same shell, it also breaks `tasklist //FO CSV`, and the game check then fails without notice.
- If a build stops with an internal compiler error (C1001), run it again. Heavy parallel builds hit it now and then on this machine.
- After adding a source file, run `python Tools/gen_projects.py` and commit the changed projects.
- Only the tests changed: `msbuild Projects/Manifold-Engine.Tests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false` is enough.

## Run

```bash
Build/x64/Release/Manifold-Engine.Tests.exe --filter=core,structures > test.log 2>&1
```

- `--list` names the suites. Without `--filter`, the run goes through every suite.
- Redirect the output into a file and search it afterwards. Piping it into `head` or `Select-Object -First` kills the tests mid-run. `--help` is not a help option: it runs everything.
- A failure far away from your change, while other builds or tests use the machine: run it once more. If it fails again, it is a finding. If it passes, write down that it failed once.
- `MANIFOLD_TEST_TRACE_FAULTS=1` writes every first-chance fault with its stack. `MANIFOLD_TEST_SUITE_MINUTES` stops a suite that hangs and dumps every thread.

## Showing a bug

1. Put the test into the existing test file of the right suite. A new file needs `gen_projects.py` and a good reason.
2. Build and run it. It must fail **for the reason the bug claims**: quote the failing line in the commit or the report. A test that fails for another reason proves nothing.
3. Make it deterministic. A race needs a forced interleaving: `--one-cpu` pinning, a hook in `TestTarget`, an event both sides wait on. A test that fails only now and then is labelled probabilistic.
4. Fix, build, run: green.
5. **Negative check:** take the fix out (keep the test), rebuild, and see the test fail again. Then put the fix back.
6. Run every suite the change touches, plus `core`. Before a merge into `main`, run the full suite.

What cannot be shown this way:
- `Source/UI` and `Application.cpp` are not built into the tests (`TEST_EXCLUDED_DIRS` in `Tools/gen_projects.py`). A UI bug needs a driven run of the program with `--home=<new folder>`, or it goes on the list as "read, not shown".
- A crash or power cut between two writes cannot be produced in a test. It also goes on the list.

## Before a commit

- The suites the change touches and `core` are green, and the negative check was done.
- Generated files are up to date: `gen_projects.py` after new files, `--errors-markdown` after a new error code, `--shortcuts-markdown` after a new command. The `docs` suite compares them.
- The diff uses LF line endings. Edit files with tools that keep them. Python `write_text` and PowerShell `Set-Content` or `Get-Content -Raw` change line endings or the encoding.
- One commit per bug, with its test. The message says what is now true.
