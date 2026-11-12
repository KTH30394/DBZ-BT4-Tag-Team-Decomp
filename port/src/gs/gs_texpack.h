/* Texture packs (gs_texpack.c): replacement textures found by PCSX2's file names. */
#ifndef PORT_GS_TEXPACK_H
#define PORT_GS_TEXPACK_H
#include <stdint.h>

#define TEXPACK_MAX_LEVELS 14

typedef struct TexPackImage {
    uint8_t *file;                            /* the whole file */
    uint32_t w, h;
    int format;                               /* 0: 8-bit R, G, B, A; 1, 2, 3: DXT1, DXT3, DXT5 */
    int levels;
    const uint8_t *data[TEXPACK_MAX_LEVELS];  /* each level's data, inside `file` */
    uint32_t bytes[TEXPACK_MAX_LEVELS];
} TexPackImage;

void TexPack_Init(void);  /* looks through the folder once */
int TexPack_Count(void);
/* The replacement for the texture the GS registers describe (as it lies in GS memory now), or -1. *maxAlpha: the largest
   alpha byte the original texture has. The game uses bit 7 of the alpha it writes into the picture as a mask for
   later draws (a texture whose alpha stays at 0x7F never sets it); a replacement's alpha is redrawn and compressed
   and strays over 0x80 in places, so the renderer keeps it at or below the original's largest value. */
int TexPack_Lookup(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t tcc, uint32_t cbp, uint32_t cpsm,
                   uint32_t *maxAlpha);
const char *TexPack_Path(int id);
/* Replacement `id`'s pixels: 1 and *img (kept by the pack: valid until the next call), 0 while the file is still
   being read (in the background: ask again later), -1 if it cannot be read. */
int TexPack_Get(int id, TexPackImage *img);
/* Anime colours (gs_draw.c, the fighters' anime pass): "<name>.anime.png" beside the pack's names, twice the texture's
   height, its top half the colour of each texel where the light falls, its bottom half where it does not; alpha 0
   where the game's own shading is kept. It may have a third block below: the texture as it was (its colours), so the
   same picture under another palette (a fighter in a fight, a flash) is known for it without having seen it before. The same name may be in several folders: forms of a fighter that share their
   textures (Goku and his Super Saiyan forms). A folder with "anime_form.txt" is a form: its colours go on the shared
   textures only while one of the textures it lists (their hashes, one a line: what only that form has, its hair, its
   face) is being drawn; a folder without it is for any fighter that has those textures.
   TexPack_AnimeFind: those for the texture of the last TexPack_Lookup, at most `max` (one a folder); none: those of the
   same texture under another palette (the game repaints a fighter's palettes for a moment when hit or charging),
   *other = 1. TexPack_AnimeFolder: one's folder; TexPack_AnimeFolderIsForm, TexPack_AnimeIsMarker: as above.
   TexPack_LoadAnime reads it now (1 = read; let go with TexPack_Free). *cut: from "anime_cut.txt" in its folder (one
   number, -100..100), how far that character's edge between light and shadow moves towards the shadow side (more of
   it lit; below 0 towards the lit side: less of it lit). */
int TexPack_AnimeFind(int *ids, int max, int *other);
/* The anime entries of the same texture as `id` in its folder under every palette there (id among them): the
   expressions of a pair of eyes, each its own palette. gs_draw.c takes the one whose original is nearest to a palette
   it has no file for. */
int TexPack_AnimeSiblings(int id, int *ids, int max);
/* The anime entries of a folder, at most `max` (gs_draw.c anime_derive: a fighter's colours read from all of them) */
int TexPack_AnimeFolderEntries(int folder, int *ids, int max);
/* The form folders (a bit each, the first 64) whose anime_form.txt lists this texture hash: a fighter drawing it is that
   form, also where the texture has no file in that folder (a folder of pieces several forms share, the gold of
   Broly's three forms, told apart by each form's own face or clothes). */
uint64_t TexPack_AnimeFoldersMarking(uint64_t tex);
uint64_t TexPack_LastTex(void); /* the texture hash the last TexPack_Lookup worked out (its name's first part), 0 = none */
int TexPack_AnimeFolder(int id);
int TexPack_AnimeFolderIsForm(int folder);
/* anime_models.txt in a folder: the file ids (decimal, one a line) of the fighter models its colours are for. Without
   it, any fighter's textures with files there take them, as before. */
int TexPack_AnimeFolderAllows(int folder, int model);
int TexPack_AnimeAnyModels(void);
int TexPack_AnimeIsMarker(int id);
/* One of the textures listed in its folder's "anime_hair.txt" (hashes, one a line): hair, drawn with a dark line along
   the edge of each lock (gs.frag mode 4). */
int TexPack_AnimeIsHair(int id);
/* One of the textures listed in its folder's "anime_hide.txt" (hashes, one a line): a layer the game draws over a
   model that already has its anime colours (Cell's gloss: the same triangles again, lit by a reflection map, added).
   Its strips are not drawn at all (gs_draw.c GsGpu_DrawVu0), so the model's anime colours and lines under it show. */
int TexPack_AnimeIsHidden(int id);
/* "anime_corte.txt" in its folder ("<hash> <0..1>" a line): where this texture's light / shadow edge goes along its
   ramp instead of where the ramp puts it (a BT4 ramp unlike the BT3 one of the same piece); -1 = as the ramp says. */
float TexPack_AnimeEdge(int id);
/* "anime_esfera.txt" in its folder ("<hash> <dark RRGGBB> <light RRGGBB>" a line): the sphere effect. The texels of this
   texture whose anime colour is the dark one get, over it, a shine of two shapes in the light colour, looked up by
   which way the surface faces the camera (gs.frag mode 4, vu0.vert), so it moves as the camera does. 1 = listed. */
int TexPack_AnimeEsfera(int id, uint32_t *osc, uint32_t *clr);
/* "anime_capas.txt" in its folder ("<hash> <w1> <w2> [<w3>]" a line): a texture of 4 tones (two middle ones) or 5 (three),
   its file 5 or 6 blocks (the tones from the lit side, then the original); w = the middle tones' widths in percent of the
   ramp, from the lit side, together centred on the edge. Returns how many middle tones (2 or 3), 0 = not listed (2 or 3
   tones as the file's height says, as before). */
int TexPack_AnimeCapas(int id, int w[3]);
/* anime_exacto.txt (Rev16, 12-nov-2026): the share of each tone in percent from the LEFT of the picture to the right, for
   a folder with anime_view.txt; gs_draw.c moves the cuts every picture so that each tone takes exactly that much of the
   piece as it is seen. Returns how many shares (0 = none: the cuts as anime_cut / anime_mid / anime_capas put them). */
int TexPack_AnimeExacto(int id, float p[5]);
/* anime_reparto.txt (Rev18, 12-nov-2026): "<hash> <cut> ..." a line, this texture's fixed cuts between its tones on the
   ramp r (0..1, from the LEFT of the picture), worked out from many pictures. Returns how many (0 = not listed). */
int TexPack_AnimeReparto(int id, float c[4]);
/* A texture name (its first part, TexPack_LastTex) that some folder's anime_gloss.txt lists; and one that the folder
   of the fighter with file `model` lists (a folder with an anime_models.txt that has it): its program 1 strips are
   not drawn even without anime colours on them (gs_draw.c). */
int TexPack_GlossAny(uint64_t tex);
int TexPack_GlossFor(uint64_t tex, int model);
/* One of the textures listed in its folder's "anime_gloss.txt": its strips drawn by program 1 (the second pass
   over a part, lit by a reflection map: Freezer's purple) are not drawn; the same texture's program 0 strips (the
   part itself, under the gloss) are, with their anime colours. For a gloss that shares its texture with the part. */
int TexPack_AnimeIsGloss(int id);
/* One of the textures listed in its folder's "anime_luz.txt": a piece the BT4 draws with program 1 (a shine over it)
   where the BT3 drew it with program 0 (Vegeta SSJ4's trousers). Its anime pass splits light and shadow by the light,
   as the program 0 pieces of the same fighter in that picture have it (vu0.vert layer 7), not by program 1's lookup
   across the picture (layer 8, the hair's), which looked like metal. */
int TexPack_AnimeLuz(int id);
/* "anime_view.txt" in its folder (a number, 1 or -1, which side; empty: 1): its light and shadow split by where the
   surface faces across the picture, as a gloss layer's reflection map splits them (Cell's), not by the light; 2 or
   -2: up and down the picture instead (the edge between them runs across: the top of a head). 0 = none. */
int TexPack_AnimeView(int id);
/* "anime_up.txt" in its folder (strip hashes, one a line: BT3_ANIME_LOG's "view strip ... uv"): those strips of a
   folder with anime_view.txt split up and down (as 2 / -2), the rest as the folder says. One part of a model that
   shares its texture with others (Freezer's head dome, his shoulder pads). */
int TexPack_AnimeUpStrip(int id, uint32_t uv);
int TexPack_LoadAnime(int id, TexPackImage *img, int *cut);
/* Three tones: a file four times the texture's height (lit, middle, shadow, the original) has a middle tone between
   light and shadow, as wide as "anime_mid.txt" in its folder says (percent of the ramp, centred on the edge: 50 = the
   middle tone half of it, light and shadow a quarter each; 30 without one). Only
   the files made that way: those of two (twice or three times the height) are drawn as before. */
int TexPack_AnimeMid(int id);
void TexPack_Free(TexPackImage *img);

#endif
