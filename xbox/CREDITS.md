# Credits

Uncharted Launcher (uncharted_xbox) runs Uncharted: The Nathan Drake Collection on the Xbox Series X|S in
Developer Mode from the player's own dump. It contains no game code or game assets.

## Built on

- **uncharted_pc** by Supermedo (Mohammed Albarghouthi) and its contributors
  (https://github.com/Supermedo/uncharted_pc): the PC port of the collection. Its native loader,
  runtime, renderer integration and in-game menu are the base of this project, which is a fork of
  it. GPL-2.0.
- **bloodborne_pc** (https://github.com/Supermedo/bloodborne_pc) and bbport by deadinside28, which
  uncharted_pc started from. GPL-2.0.
- **God of War 3 Launcher for Xbox** (https://github.com/Cosmo/gow3_xbox): the Xbox app of the
  SpartanPort fork, whose UWP host, guest memory and pager, Dozen renderer path, input, audio and
  packaging this app ports. GPL-2.0.
- **shadPS4** (https://github.com/shadps4-emu/shadPS4): the PS4 graphics emulation, its shader
  recompiler and Vulkan renderer (`gpu/shadps4`). GPL-2.0.
- **KytyPS5 for Xbox** (https://github.com/Cosmo/KytyPS5): the Xbox memory pager, the
  performance overlay and the Back-button handling follow its Xbox port. GPL-2.0.
- **Mesa / Dozen**, Vulkan on Direct3D 12, from the fork
  https://github.com/Cosmo/mesa_ai_slop_version (`vulkan_dzn.dll`). MIT.
- **DirectX Shader Compiler** (`dxil.dll`) by Microsoft. Its licenses are in `ThirdParty`.

## Libraries in the package

| Library | License |
|---|---|
| FFmpeg 7.1.1 (static, decoders the game uses) | LGPL-2.1 |
| SDL 3 | zlib |
| fmt | MIT |
| Zydis, Zycore | MIT |
| xxHash | BSD-2-Clause |
| Vulkan Memory Allocator | MIT |
| Vulkan-Headers | Apache-2.0 / MIT |
| robin-map | MIT |
| Boost (headers) | BSL-1.0 |
| LibAtrac9 | MIT |
| Dear ImGui | MIT |
| miniz | MIT |
| magic_enum | MIT |
| half | MIT |
| sirit | BSD-3-Clause |
| xbyak | BSD-3-Clause |
| FSR Vulkan | MIT |
| AMD GCN register headers | MIT (in the headers) |
| DejaVu fonts | Bitstream Vera / public domain |
| winpthreads (MSYS2 UCRT64) | MIT / ZPL |

Each license text is in the package's `ThirdParty` folder.

## License

The launcher is GPL-2.0 (`LICENSE.txt`). FFmpeg is linked statically under the LGPL-2.1: it is
ffmpeg-7.1.1 from https://ffmpeg.org, built by `xbox/scripts/build-deps.ps1` with the
configuration shown there. With this project's source you can rebuild the launcher against a
modified FFmpeg.
