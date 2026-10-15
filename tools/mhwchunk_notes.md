# MHW chunk archive format (as implemented by WorldChunkTool)

Sources (read via `gh api`, public C#):
- https://github.com/mhvuze/WorldChunkTool  (Chunk.cs, PKG.cs, Utils.cs, Resources/keyseq.key)
- https://gitee.com/wasese001/MHWNoChunk (derived tool; README only: Iceborne keys, needs oo2core_8)
- https://www.nexusmods.com/monsterhunterworld/mods/6 (WorldChunkTool page)

There is NO Blowfish in the chunk format. Encryption is a plain XOR with 16-byte keys.

## chunkX.bin
- u32 magic (`43 4d 50 00`), u32 count N, then N 8-byte entries.
- Entry: bytes 0..2 = compressed size: `(b0>>4) + (b1<<4) + (b2<<12)`; bytes 3..7 = LE u40 file offset.
  Size 0 = stored raw (0x40000 bytes).
- Every entry decodes to one 0x40000-byte block. Compressed blocks are raw Oodle Kraken (starts `8c 06`);
  third_party/ooz Kraken_Decompress decodes them (src/dst need ~64 bytes of padding).
- After decompression the block is XORed with `keys[keyseq[8+i]]` (i = block index within that chunk file,
  16-byte key repeating). 16 keys are in WorldChunkTool Utils.cs `GetChunkKey`; `keyseq[]` is the 1,000,009-byte
  Resources/keyseq.key (8-byte header, then one key index per block; compresses to ~31 KB).
  The repo has no license file, so keyseq.key is NOT vendored: mhwread reads it from MHW_KEYSEQ or build/mhw/keyseq.key.
  A shipped mod would need to fetch it or the project needs permission/derivation.
- Concatenated blocks form a virtual PKG: header at 0, u32 @0x0C = parent count, u32 @0x10 = child count,
  entries from 0x100. Parent = 0x54 bytes (name[0x3C], u64 size, u64 off, i32 type, i32 childCount@0x50).
  Child = 0xB8 bytes (name[0xA0], u64 size, u64 off, i32 type@0xB0, i32 unk). Type 0 file, 1 dir, 2 remapped (name only 0x50).
  Child `off` is an absolute offset in the virtual PKG. Paths are backslash/leading-slash style (`/common/item/itemData.itm`).

## Layering
Each chunkG*.bin and chunkHiPC/chunkG*.bin is its own PKG; the same path appears in several (patch updates).
mhwread priority (assumption, verified by sizes): base chunk dir ascending by number, then chunkHiPC ascending; last wins.
Example: `item_eng.gmd` is 479703 bytes in G0 but 515124 in G8 (the patched one). WorldChunkTool unpacks in sorted order, later overwrites.

## Findings on this install (Iceborne 15.23)
- common/item/itemData.itm 88810 B: header 10 bytes (`01 10 09 18`, u16 0x00be, u16 count=2775 @6, u16 0 @8), then 2775 records
  of 32 bytes, record u16 @0 = item id (0,1,2,...).
- common/text/steam/item_eng.gmd 515124 B (`GMD\0` magic).
- Item icon atlas: ui/common/tex/cmn_item00_ID.tex 1048768 B (HiPC copy wins; also cmn_item00M_ID.tex, cmn_micon00/01(M)_ID.tex,
  itemMenu00_ID.tex). TEX header `54 45 58 00`, 1024x1024 (0x400 at +0x18/+0x1c) BC7 blocks, 192-byte header.

## MOD3 model skeleton (`mhwread <game> BONES <path.mod3> [out.json]`, verified on m_body001_0000 / f_body001_0000)
- Every hunter equipment piece (`/pl/{m,f}_equip/plNNN_xxxx/{helm,body,arm,wst,leg}/mod/*.mod3`) and the face models carry the full
  hunter skeleton; there is no separate base-body mod3. The male body piece used here is `/pl/m_equip/pl001_0000/body/mod/m_body001_0000.mod3`.
- Header: `"MOD\0"`, u16 version 0xED @4, u16 boneCount @6 (86), u16 meshCount @8, u16 materialCount @0xA, u32 vertexCount/faceCount/...,
  then u64 offsets from 0x30: bones, groups, material names, meshes, vertex buffer, faces, ... Bone offset on this model: 0x140.
- Bone section (offset from header u64 @0x30), n = boneCount:
  - n * 24-byte entry: u16 functionId, u8 parent (0xFF = root; parent index may be greater than the child's), u8 (second link byte, NOT the
    self index), f32 unk, f32 length, f32 x, f32 y, f32 z (local translation, equals row 3 of the local matrix).
  - n * 64-byte local 4x4 (row-vector convention, translation in row 3; rotation rows are unit length).
  - n * 64-byte inverse-bind 4x4 (checked: world * invBind = identity, error < 1e-5).
  - 512-byte u8 table functionId -> bone index (0xFF none), so vertex bone ids (function ids) resolve through it.
- Units are centimetres, +Y up, rest pose is a T-pose (arms along +/-X). World = local * parent world (row vectors).
