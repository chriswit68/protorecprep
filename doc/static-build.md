# Statischer Build

`-DPRP_STATIC=ON` erzeugt unter Linux ein vollständig statisch gelinktes
Binary. Unter macOS werden nur die Drittbibliotheken statisch gelinkt;
`libc++` und `libSystem` bleiben systembedingt dynamisch, weil Apple dafür
keine statischen Bibliotheken bereitstellt (nur `.tbd`-Stubs im SDK).

## Voraussetzungen

- CMake >= 3.20 und eine C++20-Toolchain
- Boost-Header (Asio) – Boost wird nur header-only verwendet
- Ein statisches Protobuf-/Abseil-Prefix (Homebrew baut beide ausschließlich
  als Shared Libraries und ist daher nicht geeignet)

## 1. Statisches Abhängigkeits-Prefix bauen

Protobuf 36.2 benötigt Abseil 20250512.1 (siehe
`cmake/dependencies.cmake` der jeweiligen Protobuf-Version). Bei einer anderen
Protobuf-Version müssen die Versionen entsprechend angepasst werden.

```bash
export PRP_STATIC_PREFIX=/opt/prp-static
mkdir -p "$PRP_STATIC_PREFIX" build/deps
cd build/deps

curl -LO https://github.com/abseil/abseil-cpp/archive/refs/tags/20250512.1.tar.gz
tar xf 20250512.1.tar.gz
cmake -S abseil-cpp-20250512.1 -B absl-static \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DABSL_PROPAGATE_CXX_STD=ON \
  -DABSL_ENABLE_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="$PRP_STATIC_PREFIX"
cmake --build absl-static -j
cmake --install absl-static

curl -LO https://github.com/protocolbuffers/protobuf/archive/refs/tags/v36.2.tar.gz
tar xf v36.2.tar.gz
cmake -S protobuf-36.2 -B protobuf-static \
  -DCMAKE_BUILD_TYPE=Release \
  -Dprotobuf_BUILD_SHARED_LIBS=OFF \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_LIBPROTOC=ON \
  -Dprotobuf_WITH_ZLIB=OFF \
  -Dprotobuf_LOCAL_DEPENDENCIES_ONLY=ON \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_PREFIX_PATH="$PRP_STATIC_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$PRP_STATIC_PREFIX"
cmake --build protobuf-static -j
cmake --install protobuf-static
```

Wichtige Optionen:

- `protobuf_BUILD_LIBPROTOC=ON`: Die Anwendung nutzt
  `google::protobuf::compiler::Importer` und benötigt daher `libprotoc.a`.
  Upstream ist die Option standardmäßig aus.
- `protobuf_WITH_ZLIB=OFF`: Die Anwendung verwendet kein zlib; das vermeidet
  die zusätzliche Abhängigkeit von einem statischen zlib.
- `protobuf_LOCAL_DEPENDENCIES_ONLY=ON`: Verhindert, dass Protobuf Abseil
  während des Configurens aus dem Netz nachlädt.
- `CMAKE_CXX_STANDARD=17`: Abseil schreibt beim Installieren die ABI-Optionen
  in `absl/base/options.h` fest, und zwar abhängig davon, mit welchem
  C++-Standard Abseil selbst gebaut wurde. Ohne den Schalter baut Abseil in
  C++14 und pinnt `ABSL_OPTION_USE_STD_STRING_VIEW` auf `0` (eigener Typ statt
  `std::string_view`). Protobuf 36.2 wird mit C++17 gebaut und scheitert dann
  in `generated_message_reflection.cc` an `str.assign(absl::string_view)`.
  Mit C++17 pinnt Abseil auf `1` und `absl::string_view` ist ein Alias für
  `std::string_view`.
- macOS: `CMAKE_OSX_DEPLOYMENT_TARGET` muss beim Bau der Abhängigkeiten und
  der Anwendung identisch gesetzt sein.

## 2. Anwendung bauen

Das Preset liest das Präfix aus der Umgebungsvariable `PRP_STATIC_PREFIX`:

```bash
export PRP_STATIC_PREFIX=/opt/prp-static
cmake --preset static
cmake --build --preset static
```

Ohne Preset:

```bash
cmake -S . -B build/static \
  -DCMAKE_BUILD_TYPE=Release \
  -DPRP_STATIC=ON \
  -DPRP_BUILD_TESTS=OFF \
  -DCMAKE_PREFIX_PATH="$PRP_STATIC_PREFIX"
cmake --build build/static -j
```

Immer ein frisches Build-Verzeichnis verwenden: Ein gecachtes
`protobuf_DIR`/`absl_DIR` kann sonst weiter auf Homebrew zeigen. Werden
trotzdem Shared Libraries gefunden, bricht das Configuren mit einer klaren
Fehlermeldung ab.

Auf macOS wird `/opt/homebrew` weiterhin für die Boost-Header in den
Suchpfad gehängt. Die Link-Reihenfolge in `CMakeLists.txt` sorgt dafür, dass
die Include-Pfade des statischen Prefix vor `/opt/homebrew/include` liegen –
sonst würden die neueren Homebrew-Abseil-Header gegen die statische
Abseil-Bibliothek gelinkt (undefinierte Symbole wegen abweichendem
Inline-Namespace).

## 3. Verifikation

Linux:

```bash
file build/static/protorecprep    # -> statically linked
ldd build/static/protorecprep     # -> not a dynamic executable
```

macOS:

```bash
otool -L build/static/protorecprep
```

Erwartet werden nur Systempfade (`/usr/lib/...`) und Frameworks wie
CoreFoundation; es darf kein `/opt/homebrew/...` auftauchen.

Funktionstest mit dem statischen Binary:

```bash
python3 tests/integration_test.py build/static/protorecprep proto/sample.proto
python3 tests/replay_test.py build/static/protorecprep proto/sample.proto
python3 tests/passthrough_test.py build/static/protorecprep proto/sample.proto
```

## 4. Bekannte Einschränkungen

- **macOS:** Ein vollständig statisches Binary ist nicht möglich. `libSystem`
  und `libc++` müssen dynamisch gelinkt werden.
- **Linux/glibc:** `-static` erzeugt ein statisch gelinktes Binary, aber
  `getaddrinfo()` (Boost.Asio-Resolver) lädt zur Laufzeit NSS-Module per
  `dlopen`. Numerische IP-Adressen funktionieren immer; für ein wirklich
  self-contained Binary inklusive DNS ist ein musl-Toolchain-Build (z. B. in
  einem Alpine-Container) empfehlenswert. Am CMake-Aufruf ändert das nichts.
- `PRP_STATIC` ist mit `PRP_ASAN`/`PRP_TSAN` nicht kombinierbar; das
  Configuren bricht in diesem Fall ab.
