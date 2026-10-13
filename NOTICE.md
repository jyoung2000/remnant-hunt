# Credits and third-party notices

**Remnant Hunt** (Remnant II x Monster Hunter: World mashup) by Jalon, built with Claude (Anthropic).
Licensed under the GNU General Public License v3.0 or later (LICENSE). Remixes are welcome under the same license.

Monster Hunter: World is (c) CAPCOM. Remnant II is (c) Gunfire Games / Gearbox Publishing. This mod is not
affiliated with or endorsed by either. It ships none of their files: the game content shown in the mod is read from
the player's own installed copies at run time.

## Included software
| Component | Author | License | Used for |
|---|---|---|---|
| ooz (decoder files, from github.com/baconwaifu/ooz, a fork of github.com/powzix/ooz) | Powzix and contributors | GPL-3.0-or-later | `r2read.exe` decodes Remnant II's Oodle-compressed packages |
| Remnant Hunt (loader, mod, r2read) | Jalon and contributors | GPL-3.0-or-later | the mashup itself |
| Dear ImGui 1.92.5 | Omar Cornut and contributors | MIT | the mod's screens |
| MinHook | Tsuda Kageyu | BSD-2-Clause | hooks in Windows' own DLLs (dxgi, d3d12, dinput8, user32) |
| Montserrat | The Montserrat Project Authors | SIL Open Font License 1.1 | body text |
| EB Garamond | The EB Garamond Project Authors | SIL Open Font License 1.1 | titles |
| ww2ogg 0.24 (with its packed aoTuV 6.03 codebooks) | Adam Gashlin (hcs), Xiph.org Foundation | BSD-3-Clause | `r2read.exe` turns Remnant II's Wwise Vorbis sounds into standard Ogg Vorbis, in memory |
| stb_vorbis | Sean Barrett and contributors | public domain (or MIT) | `r2read.exe` decodes those sounds |
| Animation Compression Library (acl) | Nicholas Frechette and contributors | MIT | `r2read.exe` decodes Remnant II's ACL-compressed animations |
| Realtime Math (rtm) | Nicholas Frechette and contributors | MIT | math used by acl |

## Research credits
- Monster Hunter: World 15.23.00 addresses and structures: justbustin/minecraft-crossover-bridge (MIT) and
  HunterPie (Apache-2.0).
- Modding method: rehan-remade/universal-modder.
- Monster Hunter: World function patterns and object layouts (resource manager, action controller, singletons'
  GetDTI): SharpPluginLoader by Fexty12573 (MIT).
- Remnant II item numbers (gun stats, effect values, skill cooldowns): the Remnant 2 Toolkit item data by Josh Payette
  and contributors (GPL-3.0, github.com/joshpayette/remnant2-toolkit). They are re-expressed in this mod's own effect
  language and wording (tools/author_arsenal.py); no text from it is shipped. Names, descriptions and icons always come
  from the player's own Remnant II install.
- Remnant II (Unreal Engine 5.2) skeletal mesh, skeleton and animation layouts: CUE4Parse by FabianFG and
  contributors (Apache-2.0), read as a format reference; r2read's readers are this mod's own code.
