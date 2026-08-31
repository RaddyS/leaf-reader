# Leaf Reader for Chrome

A Manifest V3 companion extension that reads selected text or the primary content
of the active webpage using Leaf Reader's local Piper installation.

## Install locally

1. Open `chrome://extensions` (or `chromium://extensions`).
2. Enable **Developer mode**.
3. Choose **Load unpacked** and select this directory.

The fixed development extension ID is `nfgjhdoflejmfonoaimjoncdgmlealdi`.
From this directory, run `../scripts/install-native-host.sh` to install the
native host manifests before using speech.

The extension requests only `activeTab`, `scripting`, `storage`, and
`nativeMessaging`; it has no persistent access to browsing history or websites.
