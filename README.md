# Leaf Reader

Leaf Reader is a private, offline-first desktop book reader with a companion
Chromium extension. It renders books with Qt WebEngine and reads them aloud
using local Piper neural voices—no book text is sent to a cloud service.

## Features

- EPUB, PDF, HTML, Markdown, and plain-text support
- Modern HTML/CSS rendering with relative EPUB images, fonts, and stylesheets
- Chapter navigation and saved reading position
- Optional movable reading cursor: click in a document to choose where speech begins
- Selected-text or full-page speech
- Offline Piper neural voices with automatic model discovery
- Flite fallback through Qt Text-to-Speech
- Manifest V3 Chromium extension for reading selected text or web articles
- Shared local voices between the desktop app and browser extension

## Requirements

- Qt 6 Base, Speech, WebEngine, and XML
- `libarchive` (`bsdtar`) for EPUB extraction
- Poppler (`pdftotext`) for PDF speech text
- PipeWire (`pw-play`) for Piper playback
- Python 3.9 or newer for Piper

On Arch Linux:

```bash
sudo pacman -S --needed base-devel qt6-base qt6-speech qt6-webengine \
  flite libarchive poppler pipewire-audio python
```

## Build the desktop app

```bash
qmake6 LeafReader.pro
make -j"$(nproc)"
./leafreader
```

The generated executable and qmake build files are ignored by Git.

## Install natural Piper voices

Leaf Reader expects its private Piper environment and models under
`~/.local/share/leafreader`:

```bash
python3 -m venv ~/.local/share/leafreader/piper-venv
~/.local/share/leafreader/piper-venv/bin/pip install piper-tts
mkdir -p ~/.local/share/leafreader/voices
~/.local/share/leafreader/piper-venv/bin/python -m piper.download_voices \
  --data-dir ~/.local/share/leafreader/voices en_US-lessac-medium
```

Every voice requires matching `.onnx` and `.onnx.json` files. Leaf Reader
discovers them recursively, so additional downloaded or custom voices can be
placed anywhere below `~/.local/share/leafreader/voices`.

List the current official catalog with:

```bash
~/.local/share/leafreader/piper-venv/bin/python -m piper.download_voices
```

Review each voice's model card and license before redistribution.

## Chromium extension

The companion extension lives in [`extension/`](extension/). It requests only
`activeTab`, `scripting`, `storage`, and `nativeMessaging`. It has no persistent
website access and sends readable text only to the local Piper host.

Install the native host manifests:

```bash
./scripts/install-native-host.sh
```

Then open `chrome://extensions` or `chromium://extensions`, enable **Developer
mode**, choose **Load unpacked**, and select the repository's `extension`
directory. The fixed development extension ID is
`nfgjhdoflejmfonoaimjoncdgmlealdi`.

Select text on a normal webpage—or leave nothing selected to use the primary
article content—open Leaf Reader from the extensions menu, choose a voice, and
press **Read selection or page**.

Browser-protected pages such as `chrome://settings` cannot be accessed by any
extension. Chrome's built-in PDF viewer is similarly isolated; use the desktop
app for PDF speech.

In the desktop app, enable **Reading cursor** and click anywhere in an EPUB,
HTML, Markdown, or text document to move the visible start marker. An explicit
text selection takes priority; otherwise Read Aloud begins at the marker.

## Privacy and security

- Speech synthesis and playback happen locally.
- The extension uses a fixed ID, and the native host allow-lists only that ID.
- Native requests are validated, voice identifiers are resolved from the local
  model directory, and input length is capped.
- EPUB archives are checked for path traversal before extraction.
- Downloaded models and Python environments are intentionally not committed.

## Project structure

```text
.
├── main.cpp / readerwindow.*   Desktop Qt application
├── LeafReader.pro              qmake project
├── extension/                  Manifest V3 Chromium extension
│   └── native/                 Local Piper native-messaging host
└── scripts/                    Installation helpers
```

## Contributing

Bug reports and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md)
and [SECURITY.md](SECURITY.md).

## License

MIT. See [LICENSE](LICENSE).
