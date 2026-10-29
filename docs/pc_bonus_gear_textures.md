# PC bonus gear in Xbox DLC worlds

Issue: https://github.com/SAitken91/EVORedux/issues/5

Gold Edition's PC-exclusive outfits and bike parts occupy virtual-texture rectangles that
Xbox DLC reuses. For example, `astronaut_pants_pc_excl` (material 1254000340) uses
`U=.06640625, V=.49609375, width=.00390625, height=.00390625`. Paine Island's circus
`cylinders_b` material occupies the enclosing rectangle. Keeping both pages at the same
address displays circus textures on the space suit; incorrect normal/material channels also
explain glossy surfaces and damaged-looking gold rims.

`tools/dlc_pc_gear.py` stages a repair that moves the 85 PC-exclusive material rectangles
by +0.25 in U. It aliases 12,252 original PC texture pages, including filtering borders and
all existing mip levels, at matching unused coordinates. Base and DLC material tables agree
on the relocation, so switching worlds preserves the gear's appearance. Raw texture pixels,
mesh UVs, existing DLC page mappings and terrain assets are preserved.

The importer runs this after staging the per-world object indices. The current client shim's
base-world and DLC-world object-index hooks are required. The tool can also stage a repair
for an existing installation:

```powershell
python tools/dlc_pc_gear.py "C:\Games\TrialsPC" "C:\Temp\pc-gear-candidate"
```

Add `--install` with the game closed to install the candidate. Changed files receive a
`.before-pc-gear` backup. The manifest records input and output hashes; installation refuses
stale inputs, unexpected alias collisions, terrain collisions or changed candidate files.
Existing derived index caches are regenerated as part of staging.

Validation: unrelated archive entries remain byte-identical; only the 85 base material U
coordinates change; DLC tables gain matching coordinates; all old page mappings remain
unchanged; aliases retain original file/offset/size/flags. Repeating the conversion produces
byte-identical output. Installed locally; on 2026-10-28 the user confirmed the bonus gear
textures look correct after loading Paine Island.

This texture repair does not unlock items or edit profiles. The user's separately requested
personal bonus-gear unlock is local test setup and is not included in the importer.
