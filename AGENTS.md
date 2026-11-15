# AGENTS.md

`protorecprep` is a C++20 TCP recorder/replayer for protobuf byte streams with
four modes: `record`, `replay`, `passthrough`, `inspect`. `README.md` is the
authoritative user/CLI reference; `doc/software-design.md` (arc42) explains the
internals.

## Build and test

Host prerequisites: CMake >= 3.20, Boost >= 1.80 (headers), Protobuf with the
CMake package, `libprotoc`, and the `google/protobuf/json/json.h` API. Homebrew
Protobuf works; Ubuntu 24.04's 3.21 is too old (Docker builds Protobuf/Abseil
from source instead).

```bash
cmake --preset default          # RelWithDebInfo -> build/default
cmake --build --preset default
ctest --preset default          # 5 tests, ~30 s, loopback only
```

- Presets: `default`, `debug`, `asan`, `tsan` (build dirs under `build/`).
  Only `default`/`debug` have CTest presets; run sanitizer tests with
  `ctest --test-dir build/asan --output-on-failure`.
- One test: `ctest --test-dir build/default -R typed --output-on-failure`.
  E2E scripts also run standalone:
  `python3 tests/typed_test.py build/default/protorecprep proto/sample.proto`.
  Unit binary: `build/default/tests/prp_tests proto/sample.proto`.
- Sanitizer runs set `PRP_SKIP_PROTO_TESTS=1` because Homebrew protobuf/absl
  crashes in `DescriptorPool` under ASan/TSan. This is an intentional upstream
  workaround; do not remove it to "make tests complete".
- E2E tests use loopback TCP and temporary directories; no external services.
  `*.pcr` and `recordings/` are gitignored, so test output must not be committed.

## Architecture boundaries

- `lib/protorecrep/` is the standalone capture core: `.pcr` write/read, CRC32C,
  SHA-256, bounded queue, fs/text helpers. Keep it free of networking,
  Boost.Asio, Protobuf, CLI, JSON sidecar, and replay logic; it must build and
  install independently (`cmake -S lib/protorecrep -B ...`).
- `src/prp/` is the application layer (namespace `prp`): CLI/INI parsing,
  Asio server/client/forwarder, schema loading, JSONL writer/blob extraction,
  and the record/replay/passthrough/inspect flows.
- Public library headers live only in `lib/protorecrep/include/protorecrep/`.
  App code reaches library types through aliases in `src/prp/lib.h`; add new
  aliases there instead of renaming library symbols.
- `.pcr` is a custom versioned format, not pcap. Recorded wire bytes are stored
  verbatim (including typed 8-byte frame headers) because replay must be
  byte-exact; do not normalize/reframe on the record path.
- If the capture format changes, bump the format version and update reader and
  `inspect` truncation/recovery behavior together.

## Conventions and gotchas

- Compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (no
  `-Werror`). New code should compile warning-free; watch narrowing conversions.
- Adding/changing a CLI option requires all of: parsing plus `print_help()` in
  `src/prp/cli/options.cpp`, the README CLI tables, and tests (unit tests cover
  parsing, E2E tests cover behavior). INI keys are long option names without
  `--`; `-` and `_` are interchangeable.
- Framing rules: `--json` requires `--proto`; `typed-length-le` rejects
  `--message-type` and requires at least one `--type-map`. Raise/keep these
  validations when touching option handling.
- Docs (README, `doc/*.md`) are German; code comments, log messages, and
  `--help` text are English. Match the surrounding language.
- README mentions `tests/events.jsonl` as a reference capture, but that file
  does not exist in the repo; do not depend on it.
