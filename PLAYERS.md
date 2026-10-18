# hoi4_loader 0.5.0-beta: for players

**Disclaimer:** hoi4_loader is an unofficial tool, not affiliated with or endorsed by Paradox
Interactive. It works only at runtime: no game files are modified or distributed. Use it at your
own risk.

hoi4_loader adds new script commands to Hearts of Iron IV that some mods need. It is a
`version.dll` placed in the game folder. It changes nothing on disk: the game files stay as they
are, and it works only while the game runs.

This is a **beta**. Multiplayer is untested.

## Supported game version

- **Hearts of Iron IV 1.19.3** (Steam), as distributed or after the universal checksum patcher.
- The loader knows these builds by the SHA-256 of `hoi4.exe`, listed in `loader_config.ini`.

## Install

1. Quit the game.
2. Copy both files from `game\` into the game folder, usually
   `C:\Program Files (x86)\Steam\steamapps\common\Hearts of Iron IV\`:
   - `version.dll`
   - `loader_config.ini`

   If a `version.dll` from another tool is already there, stop: the two can't be used together
   in this release.
3. Start the game as usual. `Documents\Paradox Interactive\Hearts of Iron IV\loader.log` appears.
   Its last startup line reads `Loader ready in ... ms: hoi4_loader_version = 3, engine tier
   enabled`.

**Optional: check the download.** In a command prompt in the unpacked folder, run
`certutil -hashfile game\version.dll SHA256`. Compare the result with the line for
`game/version.dll` in `SHA256SUMS.txt`.

**Optional: the test mod.** It shows what the loader does.
1. Copy `mod\hoi4_loader_test.mod` and the folder `mod\hoi4_loader_test\` into
   `Documents\Paradox Interactive\Hearts of Iron IV\mod\`.
2. Enable **HOI4 Loader Test** in the launcher playset.
3. In a game, its decisions are in the decisions screen.

## Uninstall

1. Quit the game.
2. Delete `version.dll` and `loader_config.ini` from the game folder.
3. Optionally delete `%TEMP%\hoi4_loader\` and `%LOCALAPPDATA%\hoi4_loader\`: generated files
   and a hash cache.

**Saves:** saves made with the loader still load without it. Mods that use the loader then hide
the parts that need it. Their data is kept in the save, so it comes back when the loader is
installed again.

## After a game patch

A game update changes `hoi4.exe`, and the loader no longer recognises it.
- **Nothing crashes:** the loader switches its game-code part off by itself and the game runs
  normally.
- **Mods** see the loader's features as missing and hide what needs them. Some show a message
  saying so.
- **The log:** `loader.log` says `Unknown hoi4.exe build` and writes a signature report: how much
  of the loader still matches the new game code, and what to send.
- **What to do:** wait for a loader release that lists the new version, or send the report (see
  below) to speed that up.
- **Don't** add the new hash to `loader_config.ini` yourself unless the release notes say that
  build is fine. A matching report alone does not prove that it works.

## Reporting a problem

Open an issue at https://github.com/Builder-HD/HoI4Mod-Loader/issues and attach these from `Documents\Paradox Interactive\Hearts of Iron IV\`:
1. `loader.log`, from the session with the problem (it is rewritten at every start, so copy it
   before restarting);
2. `logs\error.log` and `logs\game.log`;
3. after a crash, the newest folder in `crashes\`;
4. the game version from the main menu (bottom left), the loader release (first line of
   `loader.log`), and the list of enabled mods;
5. what you did and what you expected.

Never send `hoi4.exe` or other game files.

## Downloads and license

New releases: https://github.com/Builder-HD/HoI4Mod-Loader/releases. hoi4_loader is open
source under the MIT license (`LICENSE.txt`). It is not affiliated with or endorsed by Paradox
Interactive.

## Multiplayer

Every player needs the same loader release and the same game build. Multiplayer has not been
tested yet in this beta.
