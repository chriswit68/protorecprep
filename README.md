# protorecprep

`protorecprep` ist ein Netzwerk-Recorder für Protobuf-Datenströme. Die Anwendung
nimmt TCP-Verbindungen an, schreibt die empfangenen Bytes verlustfrei in ein
eigenes Binärformat und kann die Aufzeichnung später byte-genau an einen anderen
Server zurückspielen. Optional entsteht parallel ein lesbares JSONL-Sidecar mit
den dekodierten Protobuf-Nachrichten.

Das Tool läuft unter macOS und Linux. Für Linux steht zusätzlich ein
Docker-Image bereit, das die Anwendung inklusive Protobuf-, Protoc- und
Abseil-Laufzeitbibliotheken enthält, sodass auf dem Host nur Docker nötig ist.

Weiterführende Dokumentation: das [arc42-Software-Design-Dokument](doc/software-design.md)
beschreibt Architektur, Laufzeitverhalten und Entscheidungen; das
[Docker-Handbuch](doc/docker.md) behandelt Containerbetrieb und Multi-Arch-Builds.

## Überblick

- **Vier Modi:** `record`, `replay`, `passthrough`, `inspect`
- **Drei Wire-Framings:** `varint-length`, `raw`, `typed-length-le` (8-Byte-Header)
- **8-Byte-Header-Protokoll:** Message-Typ-ID im Header, Zuordnung über `type_map`
- **Datei-/Blob-Kanäle:** Typ-IDs mit `@file`/`@blob` werden als Dateien extrahiert
- **Verlustfreie Binäraufnahme:** inklusive Framing-Header, exakt replayfähig
- **Optionales JSONL:** dekodierte Nachrichten, Typ-IDs, Fehlerdiagnose, Blob-Metadaten
- **JSON-Chunks:** standardmäßig eine JSONL-Datei pro Capture-Chunk für direkte Zuordnung
- **Chunking:** Aufteilung der Binäraufnahme nach Zeit und/oder Größe
- **Passthrough:** live weiterleiten und gleichzeitig aufzeichnen
- **Robuste Aufnahme:** CRC32C, Trailer, Wiederherstellung abgeschnittener Dateien

## Modi

### `record`

Startet einen TCP-Server, nimmt beliebig viele Client-Verbindungen an und
schreibt alle Bytes in Capture-Dateien. Mit `--proto` und `--json` wird
zusätzlich ein JSONL-Sidecar erzeugt. Optional können serverseitig
Protobuf-Nachrichten an verbundene Clients gesendet werden (`--send-*`).

### `replay`

Liest eine Capture-Datei oder ein ganzes Session-Verzeichnis und sendet die
aufgezeichneten Bytes an einen Zielserver. Standardmäßig geschieht das in
Echtzeit; `--speed` beschleunigt oder deaktiviert die Pacing-Logik. `--direction`
wählt die aufgezeichnete Richtung (`in`, `out`, `both`).

### `passthrough`

Verhält sich wie `record`, leitet eingehende Bytes aber zusätzlich live an einen
Zielserver weiter. Antworten des Zielservers werden an den Client
zurückgeschickt und als ausgehende Richtung mit aufgezeichnet. Es wird nie auf
die aufgezeichneten Daten zurückgegriffen.

### `inspect`

Prüft Capture-Dateien oder Session-Verzeichnisse auf Format, Header, Trailer,
CRC-Fehler und abgeschnittene Datensätze. Mit `--truncate` werden beschädigte
Dateien am letzten gültigen Datensatz abgeschnitten und so repariert.

## Schnellstart

Vorausgesetzt, das Projekt ist bereits gebaut (siehe [Bauen](#bauen)):

```bash
# Aufnahme mit Varint-Framing und JSONL-Sidecar
./build/default/protorecprep record \
  --out recordings \
  --proto proto/sample.proto \
  --message-type demo.TestRecord \
  --json

# Aufnahme mit dem 8-Byte-Header-Protokoll
./build/default/protorecprep record \
  --out recordings \
  --proto proto/sample.proto \
  --framing typed-length-le \
  --proto-package demo \
  --type-map 0=VersionInformation \
  --type-map 1=TestRecord \
  --type-map 3=@file \
  --json

# Aufzeichnung an einen anderen Server senden
./build/default/protorecprep replay \
  --dir recordings/session-20260101-120000-abcd1234 \
  --target 127.0.0.1:55556

# Live weiterleiten und gleichzeitig aufzeichnen
./build/default/protorecprep passthrough \
  --target 127.0.0.1:55556 \
  --out recordings \
  --proto proto/sample.proto \
  --message-type demo.TestRecord \
  --json

# Capture prüfen bzw. reparieren
./build/default/protorecprep inspect --dir recordings/session-20260101-120000-abcd1234
./build/default/protorecprep inspect --rec capture-000001.pcr --truncate
```

Der Server lauscht standardmäßig auf `0.0.0.0:5000`. Ein Client verbindet sich
per TCP mit diesem Port und sendet Daten im gewählten Framing.

## Wire-Framing

### `varint-length` (Standard)

Jede Nachricht besteht aus einem Varint-Längenpräfix gefolgt vom
Protobuf-Payload. Das ist das übliche „delimited“-Format vieler Protobuf-
Streams. Der Nachrichtentyp wird über `--message-type` festgelegt.

```text
[varint Länge][Protobuf-Payload]
```

### `raw`

Es werden keine Nachrichtengrenzen interpretiert. Das JSON-Sidecar schreibt die
empfangenen Bytes als `unparsed`-Event; die Binäraufnahme bleibt byte-genau.
`raw` eignet sich für unbekannte oder nicht-Protobuf-Ströme.

### `typed-length-le` (8-Byte-Header)

Jeder Frame beginnt mit zwei Little-Endian-`uint32`-Werten:

| Offset | Größe | Inhalt                                   |
|--------|-------|------------------------------------------|
| 0      | 4     | Message-Typ-ID (Little-Endian)           |
| 4      | 4     | Payload-Länge in Byte (ohne Header)      |
| 8      | n     | Protobuf-Payload oder Blob               |

Alle Typen laufen über dieselbe TCP-Verbindung. Die Zuordnung wird
ausschließlich manuell über `type_map` gepflegt:

```ini
framing = typed-length-le
proto_package = demo
type_map = 0=VersionInformation
type_map = 1=TestRecord
type_map = 2=demo.Nested
type_map = 3=@file
```

- Namen werden zuerst exakt und dann mit `proto_package` als Präfix im Schema
  gesucht und auf den kanonischen `full_name()` normalisiert.
- IDs dürfen dezimal oder hexadezimal (`0x10`) angegeben werden.
- Unbekannte IDs brechen die Verbindung nicht ab. Sie erscheinen als
  `unparsed` mit `type_id`; die Binäraufnahme bleibt vollständig.
- `--message-type` ist bei diesem Framing ungültig, da der Typ pro Frame aus
  dem Header kommt.

### Datei-/Blob-Kanäle (`@file`)

Typ-IDs, die keine Protobuf-Nachricht, sondern eine vollständige Datei
transportieren, werden mit `@file` (Alias `@blob`) konfiguriert:

```ini
type_map = 3=@file
```

Der Payload beginnt optional mit einem 4-Byte-LE-Flag (`01 00 00 00`), gefolgt
von der vollständigen Datei. Das Flag wird beim Extrahieren entfernt. Der
Dateityp wird anhand der Magic erkannt und die Datei atomar unter
`<session>/files/<seq>-<kind>.<ext>` geschrieben.

Erkannte Formate: `pcap`, `pcapng`, `wav`, `png`, `jpeg`, `gif`, `pdf`, `zip`,
`gzip`, `zstd`, `text` und `unknown`.

Im JSONL-Sidecar steht nur kompakte Metainformation:

```json
{"seq":48,"ts_ns":1760000000000000000,"stream":1,"dir":"in",
 "type":"@file","type_id":3,
 "blob":{"kind":"pcap","size":388,"sha256":"…",
         "path":"…/files/48-pcap.pcap","prefix":1}}
```

Kleine Textblobs (bis 64 KiB) werden zusätzlich als `text` eingebettet, kleine
unbekannte Blobs als `bytes_b64`. Bekannte Binärformate werden nie zusätzlich
base64-kodiert. Für die Extraktion muss `--json` aktiv sein; ohne JSON wird nur
eine Warnung ausgegeben.

### JSONL-Chunking

Ist `--json` aktiv, schreibt der JSON-Writer standardmäßig **eine JSONL-Datei
pro Capture-Chunk**. Beide Dateien teilen sich denselben Stamm:

```text
capture-000001.pcr   ↔   capture-000001.jsonl
capture-000002.pcr   ↔   capture-000002.jsonl
```

Damit lassen sich Events ohne Zeitstempel-Suche dem passenden Binär-Chunk
zuordnen. Der Recorder bestimmt die Chunk-Grenzen; der JSON-Writer übernimmt
den Segmentindex jedes Items, statt unabhängig zu rotieren. Zusätzlich enthält
jedes Event das Feld `"segment": N`.

- Chunk 1 wird beim Start angelegt, sodass auch eine leere Session eine
  JSONL-Datei besitzt.
- `seq` läuft global über die gesamte Session weiter, nicht pro Chunk.
- Ein Frame, der über eine Chunk-Grenze reicht, wird dem Item und damit dem
  Segment zugeordnet, das ihn vervollständigt.
- `--json-dir <dir>` legt die JSONL-Dateien in ein eigenes Verzeichnis.
  Ohne Angabe liegen sie direkt neben den `capture-NNNNNN.pcr`-Dateien im
  Session-Verzeichnis; relative Pfade werden relativ dazu aufgelöst.

Einzeldatei-Modus wie in früheren Versionen:

```bash
protorecprep record ... --json --no-json-chunks
protorecprep record ... --json --json-file events.jsonl   # impliziert Einzeldatei
```

`--json-file` und `--json-chunks` schließen sich gegenseitig aus. Die
Chunk-Statistik steht in `json-summary.json` unter `chunks`.

## Voraussetzungen

### Bauen auf macOS

- C++20-Compiler (Apple Clang)
- CMake ≥ 3.20
- Boost ≥ 1.80
- Protobuf mit CMake-Paket und `protoc`-Bibliothek (inkl.
  `google/protobuf/json/json.h`)

```bash
brew install cmake boost protobuf
```

### Bauen auf Linux

Benötigt werden ebenfalls CMake ≥ 3.20, Boost ≥ 1.80 und ein ausreichend neues
Protobuf. Die Protobuf-Pakete von Ubuntu 24.04 (3.21) sind zu alt für die
verwendete JSON-API. Entweder Protobuf/Abseil neuer aus dem Quellcode bauen
oder direkt das Docker-Image verwenden.

### Docker

Auf dem Host sind nur Docker Engine/Desktop und Compose v2 nötig. Alle
Laufzeitbibliotheken werden im Image mitgeliefert.

## Bauen

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Ohne Presets:

```bash
cmake -S . -B build/default -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/default -j
ctest --test-dir build/default --output-on-failure
```

Das Binary liegt danach unter `build/default/protorecprep`.

### Build-Presets

| Preset    | Verzeichnis       | Zweck                          |
|-----------|-------------------|--------------------------------|
| `default` | `build/default`   | Release mit Debug-Infos        |
| `debug`   | `build/debug`     | Debug-Build                    |
| `asan`    | `build/asan`      | Debug + AddressSanitizer       |
| `tsan`    | `build/tsan`      | Debug + ThreadSanitizer        |

### Build-Optionen

| CMake-Option       | Default | Bedeutung                    |
|--------------------|---------|------------------------------|
| `PRP_BUILD_TESTS`  | `ON`    | Tests mitbauen               |
| `PRP_ASAN`         | `OFF`   | AddressSanitizer aktivieren  |
| `PRP_TSAN`         | `OFF`   | ThreadSanitizer aktivieren   |

## CLI-Referenz

Aufruf:

```text
protorecprep <record|replay|passthrough|inspect> [Optionen]
protorecprep --help
protorecprep --version
```

Größenangaben akzeptieren `B`, `K`/`KiB`, `M`/`MiB`, `G`/`GiB`, `T`/`TiB`
(binär). Zeitangaben akzeptieren `ms`, `s`, `m`, `h`; ohne Suffix wird `ms`
angenommen. Werte ohne Suffix bei Größen sind Bytes.

### Allgemeine Optionen

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--config <file.ini>` | – | INI-Datei laden; nur einmal erlaubt, CLI-Werte haben Vorrang |
| `--quiet` | – | Nur Warnungen und Fehler loggen |
| `--verbose` | – | Debug-Logging |
| `--log-level <lvl>` | `info` | `debug`, `info`, `warn`, `error` |
| `--help`, `-h` | – | Hilfe ausgeben |
| `--version` | – | Version ausgeben |

### Record-Optionen

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--listen <host:port>` | `0.0.0.0:5000` | Bind-Adresse des Servers |
| `--out <dir>` | erforderlich | Zielverzeichnis für Aufnahmen |
| `--flat` | `false` | Direkt nach `--out` schreiben statt in ein Session-Unterverzeichnis |
| `--proto <file.proto>` | – | Protobuf-Schema für JSON und ausgehende Nachrichten |
| `--proto-path <dir>` | – | Zusätzlicher Importpfad, wiederholbar |
| `--proto-package <pkg>` | – | Paketpräfix für unqualifizierte `type_map`-Namen |
| `--message-type <pkg.Msg>` | – | Nachrichtentyp eingehender Ströme; nicht mit Typed-Framing |
| `--type-map <id>=<type>` | – | Typ-ID → Nachricht oder `@file`; wiederholbar |
| `--framing <mode>` | `varint-length` | `raw`, `varint-length`, `typed-length-le` |
| `--json[=jsonl\|off]` | `off` | JSONL-Sidecar aktivieren/deaktivieren; `--no-json` schaltet ihn ab (benötigt `--proto`) |
| `--json-chunks` | `true` | Eine `capture-NNNNNN.jsonl` pro Capture-Chunk (Default bei `--json`) |
| `--no-json-chunks` | – | Eine einzige sessionweite JSONL-Datei schreiben |
| `--json-file <path>` | – | Einzelne JSONL-Datei; impliziert `--no-json-chunks`; relative Pfade liegen in der Session |
| `--json-dir <dir>` | Session-Verzeichnis (neben den Captures) | Ablageort der JSONL-Chunks (nur im Chunk-Modus, relative Pfade ab Session-Verzeichnis) |
| `--json-print-defaults` | `false` | Felder mit Default-Werten ausgeben |
| `--json-preserve-names` | `false` | Protobuf-Feldnamen (`snake_case`) statt lowerCamelCase |
| `--max-frame <size>` | `16MiB` | Maximale Größe eines Frames (1 Byte bis 1 GiB) |
| `--recv-buffer <size>` | `256KiB` | Lesepuffer pro Verbindung (4 KiB bis 64 MiB) |
| `--queue-high-water <size>` | `256MiB` | Limit der Capture-Queue (mindestens 1 MiB) |
| `--overflow <policy>` | `block` | `block`, `drop`, `disconnect` |
| `--write-batch <size>` | `1MiB` | Schreib-Batching (4 KiB bis 256 MiB) |
| `--fsync-interval <dur>` | `1s` | Sync-Intervall; `0` = nur beim Schließen |
| `--durable` | `false` | `F_FULLFSYNC` auf macOS |
| `--rotate-size <size>` | `0` | Größenlimit pro Capture-Chunk (Alias `--chunk-size`) |
| `--rotate-interval <dur>` | `1m` | Zeitlimit pro Capture-Chunk; `0` = aus (Alias `--chunk-interval`) |
| `--io-threads <n>` | automatisch | Anzahl der IO-Threads |
| `--max-connections <n>` | `1024` | Maximale gleichzeitige Verbindungen |
| `--idle-timeout <dur>` | aus | Verbindungen nach Inaktivität schließen |

### Ausgehende Nachrichten (`--send-*`)

Nur im `record`-Modus, nicht in `passthrough`.

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--send-type <pkg.Msg>` | – | Nachrichtentyp, der an Clients gesendet wird (benötigt `--proto`) |
| `--send-content <json>` | – | JSON-Inhalt der Nachricht |
| `--send-content-file <file>` | – | JSON-Inhalt aus Datei lesen (`@file` wird ebenfalls akzeptiert) |
| `--send-once-after <dur>` | – | Einmal nach Verbindungsaufbau senden |
| `--send-every <dur>` | – | Zyklisch in diesem Intervall senden |
| `--send-count <n>` | `0` | Maximale Sends pro Verbindung; `0` = unbegrenzt |

Bei Typed-Framing wird die Typ-ID über die Rückrichtung der `type_map`
(Message-Name → ID) bestimmt.

### Replay-Optionen

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--rec <file.pcr>` | – | Einzelne Capture-Datei abspielen |
| `--dir <session-dir>` | – | Alle `capture-*.pcr` eines Verzeichnisses in Reihenfolge abspielen |
| `--target <host:port>` | erforderlich | Zielserver |
| `--speed <factor>` | `1.0` | `1.0` = Echtzeit, `2.0` = doppelt so schnell, `0` = ungebremst |
| `--direction <in\|out\|both>` | `in` | Aufgezeichnete Richtung, die gesendet wird |
| `--proto <file.proto>` | – | Optional: Schema-Digest der Aufnahme validieren |
| `--proto-path <dir>` | – | Zusätzlicher Importpfad (benötigt `--proto`) |
| `--connect-timeout <dur>` | `5s` | Timeout pro Verbindungsversuch |
| `--connect-retries <n>` | `5` | Anzahl der Verbindungsversuche |
| `--retry-interval <dur>` | `1s` | Pause zwischen Verbindungsversuchen |
| `--reconnect` | `false` | Nach Verbindungsabbruch mitten im Stream fortsetzen |
| `--max-gap <dur>` | aus | Maximale Pause zwischen Datensätzen begrenzen |
| `--strict` | `false` | Beschädigte Captures/Schema-Mismatch als fatal behandeln |
| `--io-threads <n>` | automatisch | Anzahl der IO-Threads |

`--rec` und `--dir` schließen sich gegenseitig aus; eines von beiden ist
erforderlich. `--truncate` ist im Replay ungültig, da Replay Captures nicht
verändert.

### Passthrough-Optionen

Alle Record-Optionen außer `--send-*` gelten auch hier.

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--target <host:port>` | erforderlich | Zielserver für die Live-Weiterleitung |
| `--forward-queue <size>` | `16MiB` | Backlog pro Stream und Richtung (1 MiB bis 1 GiB); bei vollem Puffer pausieren die Reads |
| `--connect-timeout <dur>` | `5s` | Timeout pro Verbindungsversuch |
| `--connect-retries <n>` | `5` | Anzahl der Verbindungsversuche |
| `--retry-interval <dur>` | `1s` | Pause zwischen Verbindungsversuchen |
| `--reconnect` | `false` | Nach Abbruch erneut zum Ziel verbinden |
| `--io-threads <n>` | automatisch | Anzahl der IO-Threads |

Das Ziel darf nicht auf den eigenen Listener zeigen. `--speed`, `--direction`,
`--max-gap`, `--strict`, `--rec`, `--dir` und `--truncate` sind hier ungültig.

### Inspect-Optionen

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `--rec <file.pcr>` | – | Einzelne Capture-Datei prüfen |
| `--dir <session-dir>` | – | Alle Capture-Dateien eines Verzeichnisses prüfen |
| `--truncate` | `false` | Beschädigte Dateien am letzten gültigen Datensatz abschneiden |

Exit-Codes von `inspect`: `0` = alles in Ordnung oder erfolgreich repariert,
`1` = beschädigt (ohne `--truncate`), `2` = Fehler beim Lesen/Reparieren.

## Konfigurationsdatei (INI)

`--config` liest dieselben Optionen aus einer INI-Datei. Regeln:

- Optionen aus der INI werden vor der Kommandozeile angewendet; explizite
  CLI-Werte gewinnen.
- Schlüssel sind die Langoptionen ohne führende `--`.
- `-` und `_` sind austauschbar (`chunk-interval` = `chunk_interval`).
- Wiederholbare Optionen (`type_map`, `proto_path`) sammeln sich an.
- Abschnitte (`[record]`, `[json]`, …) dienen nur der Lesbarkeit.
- Relative Pfade werden gegen das Arbeitsverzeichnis aufgelöst.
- `--config` darf nur einmal angegeben werden.

Beispiel:

```ini
[record]
listen = 0.0.0.0:5000
out = recordings
proto = proto/sample.proto
proto_package = demo
framing = typed-length-le
type_map = 0=VersionInformation
type_map = 1=TestRecord
# type_map = 3=@file

[json]
json = true
# Chunked JSONL (default): one capture-NNNNNN.jsonl per capture chunk.
# json_chunks = true
# json_dir = json
# Single-file mode:
# json_file = events.jsonl

[chunks]
chunk_interval = 1m
chunk_size = 0

[logging]
log_level = info
```

Fertige Beispiele liegen unter `etc/`:

| Datei | Zweck |
|-------|-------|
| `etc/example.ini` | Vollständige Beispielkonfiguration mit Kommentaren |
| `etc/typed.ini` | 8-Byte-Header-Protokoll inkl. `@file`-Hinweis |
| `etc/replay.ini` | Replay-Konfiguration |
| `etc/passthrough.ini` | Passthrough-Konfiguration |
| `etc/docker.ini` | Konfiguration für den Container |

## Ausgabe

Ohne `--flat` legt jede Aufnahme ein eigenes Session-Verzeichnis an:

```text
recordings/
└── session-20260101-120000-abcd1234/
    ├── capture-000001.pcr      # Binäraufnahme (Chunk 1)
    ├── capture-000001.jsonl    # JSONL-Events zu Chunk 1
    ├── capture-000002.pcr      # weitere Chunks bei Rotation
    ├── capture-000002.jsonl
    ├── files/                  # extrahierte Blobs (@file)
    │   ├── 48-pcap.pcap
    │   └── 49-text.txt
    ├── meta.json               # Konfiguration und Schema-Digest
    ├── summary.json            # Statistik der Binäraufnahme
    └── json-summary.json       # Statistik des JSON-Writers
```

Im Einzeldatei-Modus (`--json-file` oder `--no-json-chunks`) heißt die
JSONL-Datei standardmäßig `events.jsonl`.

Mit `--flat` liegen alle Dateien direkt in `--out`.

### `meta.json`

Enthält unter anderem Tool-Version, Modus, Session-ID, Startzeit, Host,
Listen-Adresse, Ausgabeverzeichnis, Framing, Proto-Pfad und -SHA-256,
`message_type`, `proto_package`, `type_map` (inkl. `"@file"`-Einträgen),
`blob_dir`, JSON-Einstellungen, Chunk-/IO-Parameter, `send`- und
`forward`-Konfiguration. Im Chunk-Modus stehen `json_chunks`, `json_dir` und
`json_chunk_pattern` (`capture-%06u.jsonl`) statt `json_file`.

### `summary.json`

Statistik der Binäraufnahme: `session_id`, `clean`, Start-/Endzeit, Anzahl
Segmente, Datensätze und Datenbytes, verworfene Items/Bytes, Schreibfehler und
eine Fehlermeldung.

### `json-summary.json`

Statistik des JSON-Sidecars: `events`, `messages`, `blobs`, `blob_bytes`,
`unparsed`, `parse_errors`, `dropped_items`, `dropped_bytes`,
`desynced_streams`, `skipped_data_items`, `bytes_written`, `failed`, `error`.
Im Chunk-Modus zusätzlich `chunks_written`, `chunk_open_errors` und `chunks`
(je Eintrag `segment`, `file`, `events`, `bytes`).

### JSONL-Events

Jede Zeile ist ein eigenständiges JSON-Objekt. `ts_ns` ist ein
Unix-Zeitstempel in Nanosekunden, `stream` die Verbindungs-ID, `segment` der
Capture-Chunk und `dir` die Richtung (`in` = Client → Server,
`out` = Server → Client).

| Event | Bedeutung |
|-------|-----------|
| `stream_open` | Neue Verbindung; enthält Peer und ggf. Typ |
| `message` | Dekodierte Protobuf-Nachricht |
| `unparsed` | Nicht dekodierbar; enthält Grund, ggf. `type_id` und `bytes_b64` |
| `@file` | Extrahierter Blob mit `kind`, `size`, `sha256`, `path`, ggf. `prefix`/`text`/`bytes_b64` |
| `stream_close` | Verbindung geschlossen; enthält `reason` |
| `marker` | Vom Recorder erzeugter Marker |

Beispiele:

```json
{"seq":1,"segment":1,"ts_ns":1760000000000000000,"event":"stream_open","stream":1,"peer":"192.0.2.10:40842","type":""}
{"seq":2,"segment":1,"ts_ns":1760000000000000000,"stream":1,"dir":"in","type":"demo.TestRecord","type_id":1,"message":{"seq":"1","name":"hello"}}
{"seq":3,"segment":1,"ts_ns":1760000000000000000,"stream":1,"dir":"in","unparsed":true,"reason":"unknown message type id 7","type_id":7}
{"seq":4,"segment":1,"ts_ns":1760000000000000000,"stream":1,"dir":"in","type":"@file","type_id":3,"blob":{"kind":"pcap","size":388,"sha256":"…","path":"…/files/4-pcap.pcap","prefix":1}}
{"seq":5,"segment":1,"ts_ns":1760000000000000000,"stream":1,"event":"stream_close","reason":"peer_closed"}
```

Mögliche `reason`-Werte bei `stream_close`: `peer_closed`, `local_shutdown`,
`protocol_error`, `idle_timeout`, `server_shutdown`, `overflow`,
`accept_rejected`, `send_error`.

## Capture-Format (`.pcr`)

Das Binärformat ist ein internes, versioniertes Format (kein pcap):

- Datei-Magic `PCRP`, Formatversion 1
- Header mit Session-UUID, Framing, Startzeit, SHA-256 des Protobuf-Schemas und
  einem serialisierten `FileDescriptorSet` (für die Replay-Validierung)
- CRC32C über den Header
- Datensätze mit 9-Byte-Präfix: `u8` Typ, `u32` LE Payload-Länge, `u32` LE
  CRC32C, gefolgt vom Payload
- Datensatztypen: `stream_open`, `data`, `stream_close`, `marker`, `trailer`
- Der `trailer` markiert eine saubere Beendigung
- Chunks heißen `capture-000001.pcr`, `capture-000002.pcr`, …

Die Aufnahme speichert die Wire-Bytes unverändert, inklusive des 8-Byte-Headers
bei Typed-Framing. Replay sendet sie daher exakt so, wie sie empfangen wurden.
`inspect` kann beschädigte oder abgeschnittene Dateien erkennen und bis zum
letzten gültigen Datensatz reparieren.

## Docker

```bash
docker compose build
docker compose up -d
docker compose logs -f
docker compose down
```

`compose.yaml` mountet `./recordings` (rw), `./proto` (ro) und `./etc` (ro) in
den Container und startet standardmäßig:

```bash
protorecprep record --config etc/docker.ini
```

Der veröffentlichte Port lässt sich über `PRP_PORT` ändern:

```bash
PRP_PORT=6000 docker compose up -d
```

Andere Modi einmalig starten:

```bash
# Passthrough (laufenden Record-Container vorher stoppen)
docker compose stop protorecprep
docker compose run --rm --service-ports protorecprep \
  passthrough --config etc/passthrough.ini \
  --target host.docker.internal:55556

# Replay
docker compose run --rm protorecprep \
  replay --rec /data/recordings/session-.../capture-000001.pcr \
  --target host.docker.internal:55556 --speed 1.0
```

`host.docker.internal` zeigt über `extra_hosts` auch unter Linux auf den Host.
Weitere Details, Multi-Arch-Builds und Bibliotheksprüfung stehen in
[`doc/docker.md`](doc/docker.md).

Prüfung im Image:

```bash
docker compose run --rm protorecprep --version
docker compose run --rm --entrypoint ldd protorecprep /usr/local/bin/protorecprep
```

## Tests

Die Tests laufen über CTest:

```bash
ctest --preset default
# oder
ctest --test-dir build/default --output-on-failure
```

| Test | Inhalt |
|------|--------|
| `unit` | Queue, CRC32C, Recorder/Reader, Options, Framing, JSON- und Blob-Konvertierung |
| `integration` | Record-Modus mit Varint-Framing, JSONL und Chunking |
| `replay` | Byte-genaues Replay, Pacing, Richtungen und Fehlerfälle |
| `passthrough` | Live-Weiterleitung in beide Richtungen inkl. Aufzeichnung |
| `typed` | 8-Byte-Header, Typ-Map, unbekannte IDs, Passthrough und `@file`-Blobs |

Einzelne Python-Tests lassen sich auch direkt starten:

```bash
python3 tests/typed_test.py build/default/protorecprep proto/sample.proto
python3 tests/integration_test.py build/default/protorecprep proto/sample.proto
```

`tests/events.jsonl` ist eine reale Referenzaufnahme mit 103 Blob-Frames
(59 pcap, 32 wav, 12 Text) und dient zur Gegenprüfung der Blob-Extraktion.

## Benchmark

```bash
python3 bench/benchmark.py \
  --binary build/default/protorecprep \
  --proto proto/sample.proto \
  --seconds 5
```

Das Skript vergleicht den Durchsatz von reiner Binäraufnahme und Binäraufnahme
plus JSONL-Sidecar über Loopback. Weitere Optionen: `--message-size`,
`--batch-messages`, `--queue-high-water`, `--recv-buffer`, `--write-batch`,
`--rotate-size`, `--bin-only`, `--keep`, `--out`.

## Projektstruktur

```text
src/
├── main.cpp                 # Einstiegspunkt und Kommando-Dispatch
└── prp/
    ├── app/                 # record/passthrough/inspect-Ablauf
    ├── cli/                 # Optionsparser, INI-Loader, Hilfe
    ├── core/                # Capture-Items und bounded Queue
    ├── json/                # JSONL-Writer, Blob-Extraktion
    ├── net/                 # Server, Client, Framer, Forwarder
    ├── proto/               # Schema-Laden, Descriptor-Handling
    ├── record/              # Capture-Format, Recorder, Reader
    ├── replay/              # Replay-Engine
    └── util/                # Log, CRC32C, SHA-256, Base64, FS, Text
proto/sample.proto           # Beispielschema für Tests und Beispiele
etc/                         # Beispielkonfigurationen
tests/                       # Unit- und End-to-End-Tests
bench/                       # Durchsatz-Benchmark
doc/                         # Docker-Handbuch und arc42-Design-Dokument
```

## Fehlerbehebung

**`--json requires --proto`**
Für JSON wird ein Schema benötigt, damit Nachrichten Typnamen und Felder
erhalten. `--proto` ergänzen.

**`--message-type is not allowed with typed framing`**
Bei `typed-length-le` kommt der Typ aus jedem Frame. Stattdessen
`--proto-package` und `--type-map` verwenden.

**`--json with typed framing requires at least one --type-map entry`**
Mindestens eine Typ-ID muss bekannt sein, auch wenn nur ein `@file`-Kanal
konfiguriert ist.

**Unbekannte Typ-IDs**
Sie sind kein Fehler: Der Frame erscheint als `unparsed` mit `type_id`, die
Verbindung bleibt bestehen. Die ID bei Bekanntwerden in `type_map` ergänzen.

**Blobs werden nicht extrahiert**
`@file`-Einträge benötigen `--json`. Ohne JSON wird nur eine Warnung geloggt.
Zusätzlich muss das Session-Verzeichnis beschreibbar sein.

**`message type ... not found in schema`**
Namen werden exakt und mit `proto_package` als Präfix gesucht. Schreibweise
und Paket prüfen; Importe ggf. über `--proto-path` ergänzen.

**Protobuf zu alt unter Linux**
Ubuntu 24.04 liefert 3.21, das die benötigte JSON-API nicht enthält. Docker
verwenden oder Protobuf/Abseil aus dem Quellcode bauen.

**Port bereits belegt**
`--listen` ändern oder in Docker `PRP_PORT` setzen. Beim Passthrough darf das
Ziel nicht auf den eigenen Listener zeigen.

**Replay meldet einen Schema-Mismatch**
`--proto` prüft den im Capture gespeicherten Digest. Mit dem passenden Schema
aufrufen oder `--proto` weglassen, wenn nur die Rohbytes gesendet werden
sollen.

**Beschädigte Aufnahme nach Absturz**
`inspect --rec <datei>` zeigt den Zustand; `--truncate` schneidet sie am letzten
gültigen Datensatz ab. Eine saubere Aufnahme endet mit einem `trailer`.

**Dateirechte im Container**
Der Container läuft standardmäßig als `root`. Für Host-UID-Betrieb siehe
`doc/docker.md` (`PRP_UID`/`PRP_GID`).

## Mitwirken

- Änderungen möglichst klein und thematisch getrennt halten.
- Vor einem Pull Request bauen und die vollständige Testsuite ausführen:
  `cmake --build --preset default && ctest --preset default`.
- Neue Optionen oder Formate benötigen Tests sowie eine Aktualisierung von
  `README.md`, der Hilfe in `src/prp/cli/options.cpp` und ggf. `doc/docker.md`.
- Den vorhandenen C++20-Stil und die Warnoptionen des Projekts beibehalten
  (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).

## Lizenz

Dieses Projekt steht unter der MIT-Lizenz. Siehe [LICENSE](LICENSE).
