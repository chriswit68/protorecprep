# Docker / Linux-Container

Das Image enthält die Anwendung und alle benötigten Laufzeitbibliotheken
(Protobuf, Protoc, Abseil). Auf dem Host ist nur Docker mit Compose
erforderlich. Gestartet wird standardmäßig der `record`-Modus auf Port 5000.

## Voraussetzungen

- Docker Engine oder Docker Desktop mit Compose v2
- Netzwerkzugriff während des Builds (Abseil und Protobuf werden aus dem
  Quellcode gebaut, weil die Protobuf-Pakete von Ubuntu/Debian zu alt sind)

## Bauen und starten

```bash
docker compose build
docker compose up -d
docker compose logs -f
```

Beenden:

```bash
docker compose down
```

Der Container behandelt `SIGTERM` (siehe `stop_grace_period` in
`compose.yaml`) und schließt Aufnahmen sauber ab.

## Konfiguration

`compose.yaml` mountet:

| Host              | Container         | Modus |
|-------------------|-------------------|-------|
| `./recordings`    | `/data/recordings`| rw    |
| `./proto`         | `/data/proto`     | ro    |
| `./etc`           | `/data/etc`       | ro    |

Standardmäßig läuft:

```bash
protorecprep record --config etc/docker.ini
```

`etc/docker.ini` hört auf `0.0.0.0:5000`, schreibt nach
`/data/recordings` und verwendet `/data/proto/sample.proto`.

Der veröffentlichte Port lässt sich über `PRP_PORT` ändern:

```bash
PRP_PORT=6000 docker compose up -d
```

## Andere Modi

Die Compose-Datei startet einen langlebigen `record`-Container. Für
Passthrough oder Replay wird die App einmalig mit überschriebenem Kommando
gestartet. Den laufenden Record-Container vorher stoppen, damit Port 5000
nicht doppelt belegt wird:

```bash
docker compose stop protorecprep
```

### Passthrough

`host.docker.internal` zeigt auf den Host (auf Linux über den
`extra_hosts`-Eintrag in `compose.yaml`). `--target` überschreibt das Ziel
aus der INI-Datei:

```bash
docker compose run --rm --service-ports protorecprep \
  passthrough --config etc/passthrough.ini \
  --target host.docker.internal:55556
```

`--service-ports` sorgt dafür, dass Port 5000 auch für `run` veröffentlicht
wird.

### Replay

```bash
docker compose run --rm protorecprep \
  replay \
  --rec /data/recordings/session-20260101-120000-abcd1234/capture-000001.pcr \
  --target host.docker.internal:55556 \
  --speed 1.0
```

Alternativ kann das Kommando dauerhaft in `compose.yaml` geändert werden
(`command:`).

## Typed-Header-Protokoll (8 Byte)

Für Server mit dem 8-Byte-Header wird `framing = typed-length-le` gesetzt.
Jeder Frame beginnt mit zwei Little-Endian-`uint32`:

| Offset | Größe | Inhalt                                   |
|--------|-------|------------------------------------------|
| 0      | 4     | Message-Typ-ID (Little-Endian)           |
| 4      | 4     | Payload-Länge in Byte (ohne Header)      |
| 8      | n     | Protobuf-Payload                         |

Alle Message-Typen laufen über dieselbe TCP-Verbindung; die ID im Header
bestimmt den Proto-Typ. Die Zuordnung wird ausschließlich manuell über
`type_map` gepflegt (`<id>=<Message>`). Nur ID `0` ist durch das Protokoll
festgelegt (`VersionInformation`); weitere IDs kommen mit der Zeit dazu.

`proto_package` wird als Präfix verwendet, wenn ein Name in `type_map` nicht
voll qualifiziert ist. Namen werden beim Start gegen das Schema geprüft und
auf den kanonischen `full_name()` normalisiert.

Beispiel `etc/typed.ini`:

```ini
[record]
listen = 0.0.0.0:5000
out = /data/recordings
proto = /data/proto/sample.proto
proto_package = demo
framing = typed-length-le
type_map = 0=VersionInformation
type_map = 1=TestRecord
type_map = 2=Nested

[json]
json = true
```

Im Container:

```bash
docker compose run --rm --service-ports protorecprep \
  record --config etc/typed.ini
```

Unbekannte Typ-IDs brechen die Verbindung nicht ab. Sie erscheinen im
JSONL-Sidecar als `unparsed` mit `type_id`, die Binäraufnahme bleibt
byte-genau (inklusive Header) und damit replayfähig:

```json
{"seq":3,"ts_ns":1760000000000000000,"stream":1,"dir":"in",
 "unparsed":true,"reason":"unknown message type id 7","type_id":7}
```

Bekannte Typen werden mit lesbarem Namen und ID ausgegeben:

```json
{"seq":2,"ts_ns":1760000000000000000,"stream":1,"dir":"in",
 "type":"demo.TestRecord","type_id":1,"message":{"seq":"1","name":"hello"}}
```

Die vollständige Zuordnung steht zusätzlich in `meta.json` unter `type_map`;
`proto_package` und der Modus (`framing`) sind dort ebenfalls vermerkt.

`--filter` gibt es nicht mehr: Es werden immer alle Frames aufgezeichnet.
`--message-type` ist mit Typed-Framing ungültig, weil der Typ pro Frame aus
dem Header kommt. Ausgehende `--send-*`-Nachrichten verwenden die Rückrichtung
der `type_map` (Message-Name → ID).

## Verifikation

```bash
# Binary und dynamische Bibliotheken im Image prüfen
docker compose run --rm protorecprep --version
docker compose run --rm --entrypoint ldd protorecprep /usr/local/bin/protorecprep

# Record-Modus starten
docker compose up -d
docker compose logs -f
```

Danach einen Client mit dem erwarteten Framing an `localhost:5000`
verbinden. Nach dem Beenden muss unter `recordings/` ein
`session-*`-Verzeichnis mit Capture-Dateien liegen.

## Laufzeitbibliotheken

Der Runtime-Stage kopiert `/opt/prp/lib/*.so*` aus dem Builder nach
`/opt/prp/lib` und registriert das Verzeichnis per `ldconfig`. Zusätzlich
werden nur `libstdc++6`, `libgcc-s1` und `libatomic1` installiert. Weitere
Systembibliotheken bringt das Ubuntu-Basisimage mit.

Falls `ldd` eine fehlende Bibliothek meldet, fehlt sie entweder im
`COPY --from=deps`-Schritt oder muss als Runtime-Paket ergänzt werden.

## Rechte auf Aufnahmedateien

Der Container läuft standardmäßig als `root`, damit `./recordings` ohne
UID-Mapping beschreibbar ist. Auf Linux entstehen die Dateien dann mit
Root-Besitzer. Alternativ kann der Container mit der Host-UID laufen:

```yaml
services:
  protorecprep:
    user: "${PRP_UID:-1000}:${PRP_GID:-1000}"
```

```bash
PRP_UID="$(id -u)" PRP_GID="$(id -g)" docker compose up -d
```

Das Verzeichnis `recordings/` muss in diesem Fall existieren und für diese
UID beschreibbar sein.

## Multi-Architektur

Ein lokaler Build auf Apple Silicon erzeugt ein `linux/arm64`-Image. Für
einen x86_64-Linux-Server explizit für `linux/amd64` bauen:

```bash
docker buildx build --platform linux/amd64 -t protorecprep:amd64 .
```

Oder Compose mit gesetzter Standard-Plattform verwenden:

```bash
DOCKER_DEFAULT_PLATFORM=linux/amd64 docker compose build
```

## Versionen

Die Abhängigkeitsversionen sind in `Dockerfile` und `compose.yaml` als
Build-Args hinterlegt und können überschrieben werden:

```bash
ABSEIL_VERSION=20250512.1 \
PROTOBUF_VERSION=36.2 \
docker compose build
```

Abseil und Protobuf müssen zueinander passen (Protobuf 36.2 erwartet
Abseil 20250512.1). Beide werden mit C++17 gebaut; die Anwendung selbst
verwendet C++20.
