# Bonus gear unlock

Since v1.5.4 the bonus-gear unlock is part of every build and on by default. Players can
turn it off by creating `datapack\trials_revive_no_bonus_unlock.txt`.

`client_shim/private_bonus_gear.inl` changes definitions as the game's native item
loader reads them. It does not rewrite archives, account privileges or saves.

- Remove the discontinued privilege/Uplay gates and purchase requirements from 28
  base rewards: Tron, top hat, astronaut gear, metallic torso, the Sam Fisher/HD/gold
  outfits, HD/gold bike makeovers and six pairs of bonus rims.
- Expose the two individual gold makeover rims with usable existing rim icons and
  the existing localized gold-bike label. Preserve their material/variation IDs.
- Add Banshee (bike 6) to the compatible bike sets for common stock/bonus rims and
  gold rims. Gold rims retain their original Phoenix compatibility.
- After Riders of Doom item loading finishes, route only Banshee's front/rear garage
  category references to the common rim lists. Keep its original categories and
  stock item IDs registered for existing selections. Native tables keep ownership
  of categories/items; groups borrow references as they already do on base bikes.
- Leave Gecko/Donkey categories, ordinary item prices/requirements and other packs'
  item IDs alone. Common stock upgrades retain their usual base-game prices.

Three native entry hooks are guarded against exact Steam and Ubisoft Connect code,
with translated SEH handlers. Context is thread local, scoped to the input filename,
and restored even when native parsing raises an exception.

## Validation and local installation

Evidence and reproducible checks are in
`trials_revive/hang_analysis/private_bonus/` in the enclosing workspace.

- `test_native_loader.py`: executes the compiled hooks with the real game item
  parser, constructors, native strings, compatibility hash sets and table insertion.
  The filesystem/XML DOM boundary and imported CRT services are supplied by the
  harness. Base fixture comes from the archive backup with all 28 original gates.
  Checks exact Banshee references, retained legacy stock IDs, gold icons/names,
  unchanged other bikes, unrelated-pack isolation, failure returns and x86 stacks.
- `verify_guards.py`: checks hook boundaries and SEH translation against both EXEs.
- Existing compiled DLC audio/garage and Gateways tests pass on the same DLL.
- A control build without the macro compiles and has no private hook symbols or
  private bonus log strings.

Candidate sources, map, build command and manifest are sealed in the evidence
folder's `candidate/`. `installation.json` records the live DLL path, hashes and
backup. Installed private DLL SHA256:
`dc461f33201f171fdb573ba199c1c4ef7ce528147dacbfdf33ca3f2beffd9042`.

The user confirmed the installed private build in-game: gold rims on Banshee
render correctly. Other bonus rim styles have native parser coverage; the user
did not report testing every style individually.

Rollback: close Trials, restore the DLL named by `installation.json`'s `backup`
field to its `path`, and verify its `previousHash`. That backup retains the working
Gateways/audio/texture changes. Earlier local archive unlocks remain as they were.
