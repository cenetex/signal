# Release media

`make assets` installs one reviewed ZIP pack before CMake runs. Set
`SIGNAL_ASSET_PACK_URL` to an HTTPS URL and `SIGNAL_ASSET_PACK_SHA256` to its
lowercase SHA-256. A local ZIP can be passed with
`python3 scripts/assets.py sync --pack /path/pack.zip --sha256 HASH`.

The ZIP contains paths from `assets/manifest.txt` at its root, plus
`media-provenance.json`. Every present file has a hash, source reference, and
license or permission note:

```json
{
  "schema": 1,
  "files": {
    "music/track.mp3": {
      "sha256": "64 lowercase hex characters",
      "source": "Source URL or production record",
      "license": "License name or distribution permission reference"
    }
  }
}
```

The example shows the metadata shape. The real pack must cover every required
entry in the manifest. Review the source and permission notes when selecting
the pack. Verification checks completeness and bytes against that record.

Set the same two names as GitHub repository variables for hosted builds.
Release builds require the pack. Fly builds provision it when configured.
Check the source URL, representative content types, and deployed hashes as
part of release qualification.

`python3 scripts/assets.py verify` checks an installed pack.
`python3 scripts/assets.py package --runtime native --build build --dest dist`
creates a fresh package directory. Use `--runtime web` with `--build build-web`
for the browser, or `--runtime server` for the headless server. Windows uses
`--build build/Release`.

Native archives contain the executable and `assets/`. Start the executable
from its extracted directory. Browser archives contain the four game files
and same-origin `anime/`, `music/`, and `stations/` paths. Server archives carry
the headless executable. Each archive includes `SHA256SUMS`; client archives
also include the media provenance record. The draft release adds hashes for
the complete archive set.

As of September 5, the local checkout contains the ten episode clips. The
current music and portrait source is pending. The first complete pack and its
deployment proof remain release acceptance work.
