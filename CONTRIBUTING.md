# Contributing

Thanks for your interest in improving the ESP32 Usage Monitor. This guide covers
the short version of how to get a change merged.

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)
- Python 3 (for `tools/authorize.py` and the host tests)
- For a real device: the Waveshare ESP32-S3-Touch-AMOLED-2.16 (or one of the other
  supported boards — see `src/boards/`)

## Build & test

Before opening a pull request, make sure both of these pass locally:

```bash
pio test -e native                              # host unit tests (no hardware)
pio run -e esp32-usage-monitor                  # firmware build
```

If your change touches the pure helpers under `src/util/` or the parsers under
`src/parse/`, add or update the corresponding cases in `test/test_util/`.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short summary>
```

Common types: `feat`, `fix`, `refactor`, `perf`, `docs`, `test`, `chore`.
Example: `fix(ui): stop intermittent scroll freezes`.

Keep each commit focused on one logical change.

## Never commit secrets

`data/config.json` holds OAuth credentials and is gitignored — keep it that way.
Do not paste refresh tokens, `workspace_id`s, or `waqi_token`s into issues,
commits, or example files. `data/config.json.example` must only ever contain
placeholders.

## Pull requests

- Describe **what** changed and **why**, and note the board(s) you tested on.
- Confirm `pio test -e native` and `pio run` both pass (CI runs them too).
- Update `README.md` / `AGENTS.md` / `CHANGELOG.md` when behaviour or structure
  changes.
