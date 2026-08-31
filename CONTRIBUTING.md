# Contributing

Thanks for helping improve Leaf Reader.

1. Open an issue describing a bug or proposed change.
2. Create a focused branch and keep unrelated formatting out of the change.
3. Build the desktop app with `qmake6 LeafReader.pro && make -j"$(nproc)"`.
4. Validate extension JavaScript with `node --check` and the native host with
   `python3 -m py_compile`.
5. Describe user-visible behavior and manual verification in the pull request.

Do not commit downloaded voice models, virtual environments, copyrighted books,
private recordings, or API credentials. Only contribute voice recordings and
models when every recorded speaker has explicitly consented to that use.
