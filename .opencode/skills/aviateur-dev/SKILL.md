---
name: aviateur-dev
description: Use when developing, building, rebuilding, or running this Aviateur repository; before every build it stops old running aviateur copies and after every successful build it launches the new binary.
---

# Aviateur Development Workflow

Use this skill for development tasks in this repository whenever the user asks to build, rebuild, run, test a local change by running the app, or otherwise produce a new executable.

## Required Build-Run Sequence

Always follow this order for every build:

1. Stop any old running `aviateur` process.
2. Build the current workspace.
3. If and only if the build succeeds, launch the newly built program.

## Stop Old Copies

Before building, inspect and terminate existing app processes:

```bash
pgrep -a aviateur || true
pkill -TERM -x aviateur || true
sleep 1
pkill -KILL -x aviateur || true
```

Prefer `pkill -x aviateur` so only processes whose executable name is exactly `aviateur` are targeted. Do not use broad destructive process patterns unless the user explicitly approves them.

## Build

Use the existing CMake build directory when present:

```bash
cmake --build "build" --parallel 23
```

For a clean rebuild, use:

```bash
cmake --build "build" --clean-first --parallel 23
```

If `build/` or `build/CMakeCache.txt` is missing, configure first:

```bash
cmake -S "." -B "build"
cmake --build "build" --parallel 23
```

## Run New Version

Only run the app after a successful build. Start the binary from `build/bin` in the background so relative asset paths resolve and the opencode session is not blocked:

```bash
setsid "./aviateur" > "../aviateur.log" 2>&1 < /dev/null &
```

Run that command with working directory `build/bin`.

After launching, check that it started:

```bash
pgrep -a aviateur || true
```

If launch fails or exits quickly, inspect `build/aviateur.log` and report the error.

## Reporting

In the final response, state:

- whether old copies were stopped
- whether the build succeeded
- whether the new binary was launched
- the log path: `build/aviateur.log`
