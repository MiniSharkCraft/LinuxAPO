# Configuration compatibility

The default config is `$XDG_CONFIG_HOME/skyapo/config.txt`, falling back to `~/.config/skyapo/config.txt`. Config is read at daemon start/reconnect and reloaded after edits through a debounced inotify watcher. Save the file normally or replace it atomically in the watched directory.

An opt-in build with `SKYAPO_ENABLE_FST_VST2_HOST=ON` accepts the restricted
legacy `VSTPlugin: Library "module.so" ParameterName 0.5` form. Relative
libraries resolve beside the config; values are normalized to `[0, 1]`.
`ChunkData` and Windows `.dll` loading are explicitly unsupported. The feature
is disabled in default builds and release packages pending licensing and
third-party compatibility review.

Currently implemented using actual upstream filters/processing: `Preamp:`, parametric `Filter:` (`BiQuad`), IIR `Filter:`, `Delay:`, `Channel:`, `Copy:`, `GraphicEQ:`, and `Convolution:` (the latter two require FFTW3f; convolution IR loading is Linux-specific). `Device:` uses the upstream Equalizer APO AND/OR, case-insensitive substring and GUID-matching rules against Linux PipeWire identity text: stable `node.name`, description, and available serial, bus ID and ALSA hints. Semicolon separates OR groups; whitespace-separated terms in a group are ANDed; `all` matches every device. Each Include has its own Device match scope. The daemon supplies the selected device's discovered metadata. To keep validation independent of a live PipeWire server, `skyapo config check` uses only the persisted stable node name; offline render accepts `--device <pipewire-node-name>`. Description/serial-only matches may differ in config-check filter counts from daemon runtime; without device context, only `Device: all` matches. A nonmatching Device section is intentionally filtered out and may produce a valid empty chain. When built with Lilv, `Plugin: LV2 <URI> [symbol=value ...]` inserts a native LV2 plugin with optional input control-port overrides; values must be finite and within the declared port range. `skyapo plugin info <URI>` reports the plugin name and control-port symbols/defaults/ranges. For an active LV2 node, `skyapo plugin set <URI> <symbol-or-name> <value>` changes a writable control for the next processing block; the change is runtime-only and does not rewrite the config file. `Stage:` maps the Linux capture stage to `capture`; `pre-mix` and `post-mix` sections are skipped because those are Windows APO installation stages. A child `Include:` inherits the current stage but its stage changes do not leak back to its parent. Channel selection affects later selected-channel filters; Copy uses upstream assignments and is routed through the upstream `FilterConfiguration` channel map. `Include: path` is expanded by the Linux adapter at its location in the ordered chain; relative paths resolve from the including file, double-quoted paths are accepted (including spaces and `#` characters), nesting is limited to 100 files, and active recursion cycles are diagnosed. Unquoted trailing `#` comments are removed. Child-file parser errors report that child's path and line.

With the default MuParserX 3.0.1 build, `If:`, `ElseIf:`, `Else:` and `EndIf:` support nested numeric/boolean expressions, including comparisons and semicolon sequencing. The expression variables `sampleRate`, `inputChannelCount` and `outputChannelCount` are constants from the Engine being initialized. `Eval:` assignments and numeric inline backtick expressions (for example, ``Preamp: `-6` dB``) use one parser context across nested Includes. The upstream-compatible `regexSearch` and `regexReplace` functions are available in these expressions. Upstream Equalizer APO's `+` string concatenation and infix `not` operators are also compiled from the pinned SourceForge sources in generated build-tree copies and tested through the production config path. A false branch is skipped before command validation and file inclusion; malformed nesting and expression errors include source path and line. This is a portable subset, **not** the full upstream muParserX feature set: Windows registry functions, other upstream string-operator overrides and full string/matrix expression semantics are unsupported. When built with classic muParser as the optional fallback, nested numeric/boolean conditionals work, but `Eval:` and inline backtick expressions fail explicitly with a dependency diagnostic; the upstream `+`/`not` overrides are only enabled with MuParserX.

Parsing a replacement is transactional: the daemon builds a new Engine outside the audio callback and swaps it only after successful initialization. On error the active Engine continues and `skyapo status` reports the last reload error. Copy expressions referencing unknown source channels are rejected. Copy output channels beyond the fixed input/virtual-mic layout are rejected with source/line diagnostics; same-layout remapping is supported. Unsupported directives in active stages are errors, never silently skipped; lines in a deliberately inactive Stage or conditional branch are not parsed. `LoudnessCorrection:` now reuses the actual upstream filter/factory and runs only in the daemon. Its Linux `VolumeController` adapter supplies the default PipeWire render sink's effective per-channel gain only when all channels expose the same positive gain. This is deliberately conservative: PipeWire's generic scalar volume has differed from channel gains on an ALSA sink, so SkyAPO does not claim exact equivalence to Windows `IAudioEndpointVolume`. If this live snapshot is unavailable, the filter starts neutral and updates when a usable snapshot arrives; status reports whether that input is available. Offline `skyapo-render` and `skyapo config check` reject this directive because they have no live endpoint-volume provider. Numeric/string syntax is not a complete implementation of the upstream parser; check configs with `skyapo config check FILE` before use. The current config check initializes for stereo 48 kHz, so it does not validate every device layout.

## Machine-readable validation

Use `skyapo config check --json <file>` for scripts and editors that need structured validation output. This does not change the existing text command, `skyapo config check <file>`.

A successful check exits with code `0` and prints one JSON object to stdout:

```json
{
  "valid": true,
  "file": "examples/preamp.txt",
  "filter_count": 1,
  "diagnostics": []
}
```

An invalid configuration exits with code `1`. `diagnostics` contains the parser or initialization error, including the source location when available:

```json
{
  "valid": false,
  "file": "config.txt",
  "filter_count": null,
  "diagnostics": [
    {
      "file": "/home/user/.config/skyapo/includes/voice.txt",
      "line": 4,
      "directive": "Filter: ON PK Fc 30000 Hz Gain 6 dB Q 1",
      "command": "Filter",
      "reason": "Filter frequency must be above 0 Hz and below the Nyquist frequency (24000.000000 Hz)"
    }
  ]
}
```

For an error in a nested `Include`, `diagnostics[].file` and `line` identify the included file and its line, not the parent `Include:` statement. `directive` is the trimmed source line and `command` is the text before its colon. Fields whose source information cannot be determined (for example, a missing file or a source line that cannot be read) are JSON `null`; SkyAPO does not infer a directive or line. The top-level `file` always echoes the path passed to the CLI. Both valid and invalid results are JSON on stdout; explanatory diagnostics are in the JSON `reason` field.

The CLI fixture can be run after building `skyapo`:

```sh
python3 tests/config_check_json_test.py build/skyapo
```
