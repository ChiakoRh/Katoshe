# Katoshe

Keeps selected GitHub release assets up to date on Windows. Point it at a
repository, pick assets with a wildcard, and run it again whenever you want the
newest release — it only downloads what actually changed.

- Portable: a single folder, no installer, no DLLs.
- Console (`katoshe-cli.exe`) and window (`katoshe.exe`) front ends over one
  engine, so both behave the same.
- Presets: one file per set of repositories, shareable as a file, a URL or a
  `katoshe://` link.

## Build

Needs CMake and the MSVC build tools. The CUI source tree is expected next to
this project (`../CUI`) unless you pass `-DCUI_DIR`.

```powershell
cmake -S . -B build
cmake --build build --config Release -j 2
.\build\Release\katoshe-cli.exe --version
```

## Configure

Copy the examples once:

```text
config/settings.example.json      -> config/settings.json
config/presets/default.example.json -> config/presets/default.json
```

`settings.json` holds per-machine settings and the name of the active preset.
`presets/<name>.json` holds repositories and their asset selections, and nothing
else — that is the file you share.

```json
{
  "version": 1,
  "repositories": [
    {
      "name": "whitedns",
      "url": "https://github.com/WhiteDNS/WhiteDNS-Android",
      "assets": [
        { "match": "*-arm64-v8a.apk", "filename": "whitedns.apk" }
      ]
    }
  ]
}
```

A `match` is a wildcard (`*`, `?`, `[abc]`), matched case sensitively against the
asset name. A selection that matches nothing is reported as `missing` and the
file already on disk is kept.

`mode: "archive"` keeps versions side by side (the remote file name is used) and
`keep_versions` decides how many of them stay; the default is `replace`, one
file per selection.

## Use

```text
katoshe-cli check              report what would change, writes nothing
katoshe-cli update             download and update
katoshe-cli list owner/repo    show the target release and its assets
katoshe-cli validate           parse and validate the configuration
katoshe-cli add <url>          add a repository to the active preset
katoshe-cli preset list|new|use|export|import
katoshe-cli selftest           run the built-in checks
```

Options: `--root <dir>` (config lives somewhere other than beside the exe or
`%APPDATA%\Katoshe`), `--preset <name>` (one-off run), `--token <token>`.

`Update-All.bat` runs `update` from the folder it sits in, for a shortcut or a
scheduled task.

## Sharing a preset

```text
katoshe-cli preset export work              # prints a katoshe:// link
katoshe-cli preset export work pack.json    # or writes the file
katoshe-cli preset import <link|file|url>
```

Imports never overwrite: a name that is already taken becomes `name-2`.

## Token and proxy

The token is looked up in this order: `--token`, `KATOSHE_GITHUB_TOKEN`,
`config/secrets.json` (`{ "token": "..." }`). Use a token with no scopes for
public repositories; it only raises the rate limit.

Proxy: per repository, then `KATOSHE_PROXY`, then `global.proxy`, then direct.
The default is `socks5h://127.0.0.1:10808`, so DNS is resolved on the proxy side.

## Layout

```text
katoshe-cli.exe        console front end
katoshe.exe            window front end
config/settings.json   per-machine settings
config/presets/*.json  shareable repository sets
data/lockfile.json     what was downloaded last time
downloads/             the files themselves
```

## License

MIT — see `LICENSE`.
