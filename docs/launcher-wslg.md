# WSLg graphics settings

The tracked `start_freecad.py` launcher supports an optional repository-local
`.freecad-launcher.json`. The file is ignored by Git. It changes the FreeCAD
child environment on Linux under WSLg, including normal launches,
`--authenticated-isolated`, and `--codex`. It also applies when the authenticated
isolated launcher imports the shared launcher helper directly. Launching the
FreeCAD executable or a Pixi task directly bypasses this helper.

To select Mesa's D3D12 backend and a particular GPU, create this file in the
FreeCAD repository root, replacing the adapter value with the full device name:

```json
{
  "wslg": {
    "renderer": "d3d12",
    "adapter": "<full adapter name>"
  }
}
```

Subsequent `python3 start_freecad.py` launches read it automatically; no repeated
graphics flags or shell configuration are needed. The launcher passes
`GALLIUM_DRIVER=d3d12` and `MESA_D3D12_DEFAULT_ADAPTER_NAME` to FreeCAD. The adapter
is optional, but omitting it lets Mesa choose a GPU. Mesa uses a name match, so a
broad value such as `NVIDIA` does not distinguish two NVIDIA adapters. Confirm the
renderer in the actual FreeCAD viewport after restarting; device visibility or
the settings file alone does not establish which GPU renders it. See
[Mesa's D3D12 adapter selection](https://docs.mesa3d.org/drivers/d3d12.html#utilities).

Use `"renderer": "software"` for a persistent software-rendering opt-out. This
selects `GALLIUM_DRIVER=llvmpipe`; an existing adapter value can stay in the file
for a later switch back. For a single launch, use:

```sh
GALLIUM_DRIVER=llvmpipe python3 start_freecad.py
```

Explicit caller `GALLIUM_DRIVER` or `MESA_LOADER_DRIVER_OVERRIDE` takes precedence
over the local policy, including an explicitly empty value. An existing
`MESA_D3D12_DEFAULT_ADAPTER_NAME` takes precedence over the local adapter name.
When the policy is enabled and no caller driver selection exists,
`LIBGL_ALWAYS_SOFTWARE=1` (also `true`, `yes`, or `on`) selects llvmpipe. This
explicit handling matters because the tested Mesa software-winsys path still
uses D3D12 when both `LIBGL_ALWAYS_SOFTWARE=1` and `GALLIUM_DRIVER=d3d12` are set.
If the caller explicitly supplies both, the launcher preserves those values.

Set `"renderer": "auto"` or remove `.freecad-launcher.json` to stop adding any
graphics settings. With no file, the launcher retains its previous graphics
selection behavior. Other operating systems and environments without the WSLg
runtime directory ignore this policy. Invalid JSON or invalid `wslg` settings
produce an error identifying the settings file.

This policy changes neither the parent shell nor FreeCAD's saved preferences,
drivers, Mesa, Qt, or system settings. In the diagnosed Mesa 25.2.8 runtime,
the software winsys tries llvmpipe first when `GALLIUM_DRIVER` is unset; selecting
D3D12 changes that choice. The installed Ubuntu source is available at
[the matching Mesa source revision](https://git.launchpad.net/ubuntu/+source/mesa/plain/src/gallium/auxiliary/target-helpers/sw_helper.h?id=4e1f6bf2179734776266e2d6b76c954ac0feeac6).
FreeCAD's `UseSoftwareOpenGL` preference sets a Qt application attribute, whose
documented implementation-selection behavior depends on dynamic OpenGL loading.
That feature is disabled in the inspected Linux Qt build. Changing that
preference is not the established Mesa selection repair; see
[Qt's application attributes](https://doc.qt.io/qt-6.11/qt.html#ApplicationAttribute-enum).

Run the focused regression checks with:

```sh
python3 -m unittest discover -s tests/launcher -p test_start_freecad_wslg.py -v
```
