/*
 * Texture packs: replacement textures in the naming of PCSX2's texture replacement, so packs made for the game
 * under that emulator work as they are.
 *
 * A file is named <texture hash>-<palette hash>-<bits>.dds (or .png) (two parts, without the palette hash, for a texture
 * without a palette), the hashes in hexadecimal without leading zeros:
 *   texture hash   XXH3 (64 bits) over the texture's raw 256-byte blocks of GS memory, block after block in
 *                  row-major block order over the texture's rectangle: the data as it lies in GS memory
 *   palette hash   XXH3 over the palette's 32-bit entries in index order, from the lowest to the highest index
 *                  the texture uses (some files: over the whole palette; both are looked up)
 *   bits           PSM | log2(width) << 6 | log2(height) << 10 | TCC << 14
 * Found by matching a pack against the textures the game uploads (docs/port/README.md). The files are looked for
 * at any depth below the folder `textures` next to the program (or the one BT3_TEXTURES names). A pack's alpha
 * is the GS's own (0x80 = opaque), like the textures it replaces.
 *
 * Read: DDS with DXT1 / DXT3 / DXT5 data or plain 32-bit pixels, with its smaller copies (mip levels), and PNG
 * (the smaller copies are made here). Where a name exists as both, the first one found is used.
 *
 * The files are read by a thread of their own (TexPack_Get), the way PCSX2 loads them "asynchronously": a file read
 * inside a frame held the picture up for as long as the disk took, 30 to 150 ms when an ultimate's effects first
 * appear, and on a machine whose memory does not keep the pack's files cached every time the game uploads one of
 * its textures again (hundreds of times a fight). Until a file is in, the renderer draws the original texture. What
 * was read stays in memory (TEXPACK_BUDGET, the least recently used files go first). BT3_PACK_SYNC=1: each file is
 * read at once, inside the frame, as before (the same picture in every run: tests).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"
#include "gs_internal.h"
#include "gs_texpack.h"
#include "../../include/bt4_texture_pack.h"
#include "gs_index_bounds.h"
/* The updater already links miniz's read-only API. Supply bounded SDL I/O
   here so multi-gigabyte packs need neither a RAM copy nor disk extraction. */
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../third_party/miniz/miniz.h"

typedef struct ZipPack { mz_zip_archive zip; SDL_IOStream *io; } ZipPack;
static ZipPack sZip[32];
static int sZipCount;

typedef struct Entry {
    uint64_t tex, clut;
    uint32_t bits;
    int pal;
    int anime;        /* <name>.anime.png: the lit and shadow colours of the texture's texels (TexPack_AnimeFind) */
    int folder;       /* and then its folder's settings (sFolders) */
    char *path;
    int archive;      /* -1 for a loose file, otherwise an index into sZip */
    mz_uint member;
    int state;        /* 0 not in memory, 1 waiting for the reader, 2 in memory (img), 3 unreadable */
    unsigned used;    /* when TexPack_Get last asked for it */
    size_t held;      /* the bytes img holds */
    TexPackImage img;
} Entry;

static Entry *sEntries;
static uint32_t sCount, sCap, *sBucket, sBuckets;

#define TEXPACK_BUDGET ((size_t)512 << 20) /* CPU images, separate from the decoded renderer working set. */
#define QUEUE 4096                          /* files waiting for the reader (a power of two) */
static SDL_Mutex *sLock;
static SDL_Condition *sWake;
static SDL_Thread *sReader;
static uint32_t sQueue[QUEUE], sHead, sTail; /* the reader takes from sTail */
static uint32_t *sHeld, sHeldCount;          /* the entries in memory (state 2) */
static size_t sHeldBytes, sBudget;
static unsigned sTick;
static int sSync = -1;
/* The .anime.png entries (each kept, also where another folder has the same name: one per form of a fighter), their
   folders' settings, and the texture of the last TexPack_Lookup as TexPack_AnimeFind compares it. */
static uint32_t *sAnimeIds, sAnimeCount;
typedef struct AnimeFolder {
    char *dir;
    int cut;           /* anime_cut.txt */
    int mid;           /* anime_mid.txt: how much of the ramp the middle tone of a three tone texture takes (percent, default 30) */
    int form;         /* anime_form.txt is there: a form of a fighter (TexPack_AnimeFolderIsForm) */
    uint64_t *markers; /* and the texture hashes in it: textures only that form has (TexPack_AnimeIsMarker) */
    int markerCount;
    uint64_t *hair;    /* anime_hair.txt: the texture hashes of the hair (TexPack_AnimeIsHair) */
    int hairCount;
    uint64_t *hide;    /* anime_hide.txt: textures whose strips are not drawn (TexPack_AnimeIsHidden) */
    int hideCount;
    uint64_t *gloss;   /* anime_gloss.txt: textures whose program 1 pass (the gloss) is not drawn (TexPack_AnimeIsGloss) */
    int glossCount;
    uint64_t *luz;     /* anime_luz.txt: textures whose program 1 strips split light and shadow by the light (TexPack_AnimeLuz) */
    int luzCount;
    uint64_t *up;      /* anime_up.txt: strips (their texture coordinates' hash, gs_draw.c) split up and down */
    int upCount;
    int view;          /* anime_view.txt: 1 / -1 across the picture, 2 / -2 up and down it, the sign the side; 0 = none */
    int brillo;        /* (tenk4) anime_brillo.txt: program 1's anime pass takes the colours where layer 5 looks its
                          reflection map up (the tone block holds that map drawn in the folder's colours), no lines */
    int *models;       /* anime_models.txt: the fighter models (file ids) the folder is for; none = any (TexPack_AnimeFolderAllows) */
    int modelCount, hasModels;
    uint64_t *edgeTex; /* anime_corte.txt: textures whose light / shadow edge is fixed (TexPack_AnimeEdge), and where */
    float *edgeAt;
    int edgeCount;
    uint64_t *esfTex;  /* anime_esfera.txt: textures with the sphere effect (TexPack_AnimeEsfera), their dark (base) and */
    uint32_t *esfOsc, *esfClr; /* light (the two-shape shine) colours, 0xRRGGBB */
    int esfCount;
    uint64_t *capTex;  /* anime_capas.txt: textures of 4 or 5 tones (TexPack_AnimeCapas) and the widths of their middle */
    int (*capW)[3];    /* tones, in percent of the ramp, from the lit side (2 for 4 tones, 3 for 5) */
    int *capN, capCount;
} AnimeFolder;

/* A file of texture hashes, one a line (hexadecimal; anything after it ignored). 1 = it is there. */
static int read_hashes(const char *path, uint64_t **list, int *count) {
    char line[256];
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *end;
        unsigned long long v = strtoull(line, &end, 16);
        if (end != line) {
            *list = realloc(*list, (size_t)(*count + 1) * sizeof(uint64_t));
            (*list)[(*count)++] = (uint64_t)v;
        }
    }
    fclose(f);
    return 1;
}
static AnimeFolder *sFolders;
static int sFolderCount;
static struct { uint64_t tex, clutUsed, clutAll; uint32_t bits; int pal, valid; } sLookup;

static int folder_of(const char *dir) {
    char path[1200];
    FILE *f;
    int i;
    AnimeFolder *a;

    for (i = 0; i < sFolderCount; i++) {
        if (strcmp(sFolders[i].dir, dir) == 0) {
            return i;
        }
    }
    sFolders = realloc(sFolders, (size_t)(sFolderCount + 1) * sizeof(AnimeFolder));
    a = &sFolders[sFolderCount];
    memset(a, 0, sizeof(*a));
    a->dir = malloc(strlen(dir) + 1);
    strcpy(a->dir, dir);
    snprintf(path, sizeof(path), "%sanime_cut.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &a->cut) != 1 || a->cut < -100 || a->cut > 100) {
            a->cut = 0;
        }
        fclose(f);
    }
    a->mid = 30;
    snprintf(path, sizeof(path), "%sanime_mid.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &a->mid) != 1 || a->mid < 0 || a->mid > 100) {
            a->mid = 30;
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_form.txt", dir);
    a->form = read_hashes(path, &a->markers, &a->markerCount);
    snprintf(path, sizeof(path), "%sanime_hair.txt", dir);
    read_hashes(path, &a->hair, &a->hairCount);
    snprintf(path, sizeof(path), "%sanime_hide.txt", dir);
    read_hashes(path, &a->hide, &a->hideCount);
    snprintf(path, sizeof(path), "%sanime_gloss.txt", dir);
    read_hashes(path, &a->gloss, &a->glossCount);
    snprintf(path, sizeof(path), "%sanime_luz.txt", dir);
    read_hashes(path, &a->luz, &a->luzCount);
    snprintf(path, sizeof(path), "%sanime_up.txt", dir);
    read_hashes(path, &a->up, &a->upCount);
    snprintf(path, sizeof(path), "%sanime_models.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        char line[256];
        a->hasModels = 1;
        while (fgets(line, sizeof(line), f) != NULL) {
            char *end;
            long v = strtol(line, &end, 10);
            if (end != line && v > 0) {
                a->models = realloc(a->models, (size_t)(a->modelCount + 1) * sizeof(int));
                a->models[a->modelCount++] = (int)v;
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_corte.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) { /* "<texture hash> <edge 0..1>" a line: BT4 ramps that put the edge elsewhere than BT3's did */
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *end, *end2 = NULL;
            unsigned long long v = strtoull(line, &end, 16);
            double e = end != line ? strtod(end, &end2) : -1.0;
            if (end != line && end2 != NULL && end2 != end && e >= 0.0 && e <= 1.0) {
                a->edgeTex = realloc(a->edgeTex, (size_t)(a->edgeCount + 1) * sizeof(uint64_t));
                a->edgeAt = realloc(a->edgeAt, (size_t)(a->edgeCount + 1) * sizeof(float));
                a->edgeTex[a->edgeCount] = (uint64_t)v;
                a->edgeAt[a->edgeCount++] = (float)e;
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_esfera.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) { /* "<texture hash> <dark RRGGBB> <light RRGGBB>" a line: the sphere effect (12-nov-2026, Broly) */
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *end, *end2 = NULL, *end3 = NULL;
            unsigned long long v = strtoull(line, &end, 16);
            unsigned long o = end != line ? strtoul(end, &end2, 16) : 0, c = end2 != NULL && end2 != end ? strtoul(end2, &end3, 16) : 0;
            if (end != line && end2 != NULL && end2 != end && end3 != NULL && end3 != end2 && o <= 0xFFFFFFul && c <= 0xFFFFFFul) {
                a->esfTex = realloc(a->esfTex, (size_t)(a->esfCount + 1) * sizeof(uint64_t));
                a->esfOsc = realloc(a->esfOsc, (size_t)(a->esfCount + 1) * sizeof(uint32_t));
                a->esfClr = realloc(a->esfClr, (size_t)(a->esfCount + 1) * sizeof(uint32_t));
                a->esfTex[a->esfCount] = (uint64_t)v;
                a->esfOsc[a->esfCount] = (uint32_t)o;
                a->esfClr[a->esfCount++] = (uint32_t)c;
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_capas.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) { /* "<texture hash> <w1> <w2> [<w3>]" a line: 4 tones (two middle ones) or 5 (three), 12-nov-2026 */
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *end, *p;
            int w[3] = {0, 0, 0}, k = 0, ok = 1;
            unsigned long long v = strtoull(line, &end, 16);
            if (end == line) {
                continue;
            }
            p = end;
            while (k < 3) {
                char *e2;
                long x = strtol(p, &e2, 10);
                if (e2 == p) {
                    break;
                }
                if (x < 0 || x > 99) {
                    ok = 0;
                }
                w[k++] = (int)x;
                p = e2;
            }
            if (ok && k >= 2 && w[0] + w[1] + w[2] <= 100) {
                a->capTex = realloc(a->capTex, (size_t)(a->capCount + 1) * sizeof(uint64_t));
                a->capW = realloc(a->capW, (size_t)(a->capCount + 1) * sizeof(*a->capW));
                a->capN = realloc(a->capN, (size_t)(a->capCount + 1) * sizeof(int));
                a->capTex[a->capCount] = (uint64_t)v;
                a->capW[a->capCount][0] = w[0];
                a->capW[a->capCount][1] = w[1];
                a->capW[a->capCount][2] = k == 3 ? w[2] : 0;
                a->capN[a->capCount++] = k;
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_view.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &a->view) != 1 || a->view == 0) {
            a->view = 1;
        }
        a->view = a->view > 0 ? (a->view >= 2 ? 2 : 1) : (a->view <= -2 ? -2 : -1);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sanime_brillo.txt", dir);
    f = fopen(path, "r");
    if (f != NULL) {
        a->brillo = 1;
        fclose(f);
    }
    return sFolderCount++;
}

static uint32_t bucket_of(uint64_t tex, uint64_t clut, uint32_t bits) {
    uint64_t h = (tex ^ (clut * 0x9E3779B97F4A7C15ull) ^ bits) * 0x9FB21C651E98DF25ull;
    return (uint32_t)(h >> 40);
}

static int find(uint64_t tex, uint64_t clut, uint32_t bits, int pal, int anime) {
    uint32_t h = bucket_of(tex, clut, bits) & (sBuckets - 1);
    while (sBucket[h] != 0) {
        const Entry *e = &sEntries[sBucket[h] - 1];
        if (e->tex == tex && e->clut == clut && e->bits == bits && e->pal == pal && e->anime == anime) {
            return (int)sBucket[h] - 1;
        }
        h = (h + 1) & (sBuckets - 1);
    }
    return -1;
}

/* TCC controls whether a draw uses texture alpha, not the texture's pixels.
   PCSX2 packs may record the same image with the opposite TCC. Prefer the
   exact entry and keep palette, format and dimensions mandatory in both cases. */
static int find_image(uint64_t tex, uint64_t clut, uint32_t bits, int pal) {
    int id = find(tex, clut, bits, pal, 0);
    return id >= 0 ? id : find(tex, clut, bits ^ 0x4000u, pal, 0);
}

/* "1dd4c76113969303-56e3d4469d2ad392-00005e54.dds" (or "....anime.png": TexPack_AnimeFind) */
static int sOnlyAnime; /* (TexPack_Init) the managed HD archive is the pack: from the folder only the anime colours */
static void add(const char *dir, const char *name, int archive, mz_uint member) {
    size_t n = strlen(name), stem;
    char part[3][20];
    int parts = 0, len = 0, anime = 0;
    size_t i;
    Entry e;

    if (n < 8 || (SDL_strcasecmp(name + n - 4, ".dds") != 0 && SDL_strcasecmp(name + n - 4, ".png") != 0)) {
        return;
    }
    stem = n - 4;
    if (n > 14 && SDL_strcasecmp(name + n - 10, ".anime.png") == 0) {
        anime = 1;
        stem = n - 10;
    }
    if (sOnlyAnime && !anime && archive < 0) {
        return;
    }
    for (i = 0; i < stem; i++) {
        char c = name[i];
        if (c == '-') {
            if (len == 0 || parts == 2) {
                return;
            }
            part[parts++][len] = '\0';
            len = 0;
        } else if (((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) && len < 16) {
            part[parts][len++] = c;
        } else {
            return; /* not one of these names */
        }
    }
    if (len == 0 || parts == 0) {
        return;
    }
    part[parts++][len] = '\0';
    e.tex = strtoull(part[0], NULL, 16);
    if(getenv("BT4_PTBR_ROOT")){
        static uint64_t blocked[1024];static unsigned count;static int loaded;
        if(!loaded){char path[2048],line[80];FILE *f;loaded=1;
            SDL_snprintf(path,sizeof(path),"%s/fontes-sem-conflito.txt",getenv("BT4_PTBR_ROOT"));f=fopen(path,"rb");
            if(f){while(count<1024&&fgets(line,sizeof(line),f))blocked[count++]=strtoull(line,NULL,16);fclose(f);}}
        for(unsigned k=0;k<count;k++)if(blocked[k]==e.tex)return;
    }
    e.pal = parts == 3;
    e.clut = e.pal ? strtoull(part[1], NULL, 16) : 0;
    e.bits = (uint32_t)strtoul(part[parts - 1], NULL, 16);
    e.anime = anime;
    e.folder = anime ? folder_of(dir) : -1;
    e.state = 0;
    e.archive = archive;
    e.member = member;
    e.used = 0;
    e.held = 0;
    memset(&e.img, 0, sizeof(e.img));
    e.path = malloc(strlen(dir) + n + 1);
    strcpy(e.path, dir);
    strcat(e.path, name);
    if (sCount == sCap) {
        sCap = sCap ? sCap * 2 : 4096;
        sEntries = realloc(sEntries, sCap * sizeof(Entry));
    }
    sEntries[sCount++] = e;
}

static size_t zip_read(void *opaque,mz_uint64 offset,void *buffer,size_t bytes){
    SDL_IOStream *io=opaque;
    if(offset>INT64_MAX||SDL_SeekIO(io,(Sint64)offset,SDL_IO_SEEK_SET)!=(Sint64)offset)return 0;
    return SDL_ReadIO(io,buffer,bytes);
}
static void add_zip(const char *path){
    ZipPack *pack;Sint64 bytes;mz_uint i,count;uint32_t before=sCount;
    if(sZipCount==(int)(sizeof(sZip)/sizeof(sZip[0])))return;
    pack=&sZip[sZipCount];pack->io=SDL_IOFromFile(path,"rb");if(!pack->io)return;
    bytes=SDL_GetIOSize(pack->io);pack->zip.m_pRead=zip_read;pack->zip.m_pIO_opaque=pack->io;
    if(bytes<=0||!mz_zip_reader_init(&pack->zip,(mz_uint64)bytes,0)){
        SDL_CloseIO(pack->io);memset(pack,0,sizeof(*pack));return;
    }
    count=mz_zip_reader_get_num_files(&pack->zip);
    for(i=0;i<count;i++){
        mz_zip_archive_file_stat st;const char *name,*base;char *dir;size_t prefix;
        if(!mz_zip_reader_file_stat(&pack->zip,i,&st)||st.m_is_directory||st.m_is_encrypted||st.m_uncomp_size>(128u<<20))continue;
        name=st.m_filename;base=strrchr(name,'/');base=base?base+1:name;
        prefix=strlen(path)+2+(size_t)(base-name);dir=malloc(prefix+1);if(!dir)continue;
        SDL_snprintf(dir,prefix+1,"%s!/%.*s",path,(int)(base-name),name);
        add(dir,base,sZipCount,i);free(dir);
    }
    fprintf(stderr,"bt4: ZIP texture pack: %u textures in %s (read on demand)\n",sCount-before,path);
    sZipCount++;
}

static SDL_EnumerationResult SDLCALL walk(void *userdata, const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *path = malloc(n);
    SDL_PathInfo info;

    (void)userdata;
    if(SDL_strcasecmp(name,"PTBR")==0&&!getenv("BT4_PTBR_ROOT")){free(path);return SDL_ENUM_CONTINUE;}
    snprintf(path, n, "%s%s", dir, name);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        strcat(path, "/");
        SDL_EnumerateDirectory(path, walk, NULL);
    } else {
        size_t len=strlen(name);
        if(len>4&&SDL_strcasecmp(name+len-4,".zip")==0){ if(!sOnlyAnime)add_zip(path); }
        else add(dir, name, -1, 0);
    }
    free(path);
    return SDL_ENUM_CONTINUE;
}

void TexPack_Init(void) {
    char nextToProgram[2048];
    const char *root = getenv("BT3_TEXTURES");
    if(root==NULL){
        const char *base=SDL_GetBasePath();SDL_PathInfo at;
        SDL_snprintf(nextToProgram,sizeof(nextToProgram),"%stextures",base?base:"");
        root=SDL_GetPathInfo(nextToProgram,&at)&&at.type==SDL_PATHTYPE_DIRECTORY?nextToProgram:"textures";
        if(!SDL_GetPathInfo(root,&at)&&getenv("BT4_PTBR_ROOT")){
            SDL_snprintf(nextToProgram,sizeof(nextToProgram),"%s/textures",getenv("BT4_PTBR_ROOT"));
            root=nextToProgram;
        }
    }
    size_t n = strlen(root);
    char *dir = malloc(n + 2);
    uint32_t i, dup = 0;
    SDL_PathInfo info;

    if (root[0] == '\0' || !SDL_GetPathInfo(root, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        free(dir);
        return;
    }
    strcpy(dir, root);
    if (dir[n - 1] != '/' && dir[n - 1] != '\\') {
        strcat(dir, "/");
    }
    if(getenv("BT4_PTBR_ROOT")){
        char portuguese[2048];SDL_snprintf(portuguese,sizeof(portuguese),"%s/textures/",getenv("BT4_PTBR_ROOT"));
        SDL_EnumerateDirectory(portuguese,walk,NULL);
    }
    {
        char selected[4096];SDL_PathInfo selectedInfo;
        SDL_snprintf(selected,sizeof(selected),"%s%s",dir,BT4_TEXTURE_ARCHIVE);
        /* The managed R3 archive replaces the preceding pack as a whole.
           Leave other files intact, but do not mix their missing entries into
           the selected pack. Portuguese profile overrides remain isolated. */
        if(SDL_GetPathInfo(selected,&selectedInfo)&&selectedInfo.type==SDL_PATHTYPE_FILE){
            add_zip(selected);
            /* (anime: the folders' .anime.png and their .txt still count with the HD pack; other loose textures
               stay out of it, as the pack's authors want; zips in the folder too) */
            sOnlyAnime=1;
            SDL_EnumerateDirectory(dir, walk, NULL);
            sOnlyAnime=0;
        }
        else SDL_EnumerateDirectory(dir, walk, NULL);
    }
    free(dir);
    if (sCount == 0) {
        return;
    }
    for (sBuckets = 1024; sBuckets < sCount * 2; sBuckets *= 2) {
    }
    sBucket = calloc(sBuckets, sizeof(uint32_t));
    for (i = 0; i < sCount; i++) {
        const Entry *e = &sEntries[i];
        uint32_t h = bucket_of(e->tex, e->clut, e->bits) & (sBuckets - 1);
        if (e->anime) { /* (not in the buckets: TexPack_AnimeFind goes through all of them) */
            sAnimeIds = realloc(sAnimeIds, (sAnimeCount + 1) * sizeof(uint32_t));
            sAnimeIds[sAnimeCount++] = i;
            continue;
        }
        if (find(e->tex, e->clut, e->bits, e->pal, e->anime) >= 0) {
            dup++; /* the same name in two folders of the pack: the first one found is used */
            continue;
        }
        while (sBucket[h] != 0) {
            h = (h + 1) & (sBuckets - 1);
        }
        sBucket[h] = i + 1;
    }
    fprintf(stderr, "bt3: texture pack: %u replacement textures in %s (%u more with a name already seen)\n", sCount - dup, root, dup);
    fprintf(stderr, "bt4 anime: build tenk5 (14-nov-2026, rev b; todas las lineas por el codigo, caras incluidas; brillo de Freezer con su dibujo; museo: R2/L2 zoom y cruceta para mover el modelo; tenk1 + contorno y uniones entre partes con el grosor y borde suave de las lineas de dentro; capas metalicas de Cell, Cell 2a y pantalon de Vegeta SSJ4 ocultas por nombre; texturas sin fila de luchador con sus colores anime: brillo de Freezer con sus 3 colores)\n");
}

int TexPack_Count(void) {
    return (int)sCount;
}

const char *TexPack_Path(int id) {
    return id >= 0 && (uint32_t)id < sCount ? sEntries[id].path : NULL;
}

int TexPack_Lookup(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t tcc, uint32_t cbp, uint32_t cpsm, uint32_t *maxAlpha) {
    uint32_t bw, bh, x, y, i, lo = 255, hi = 0, bits, wl = 0, hl = 0;
    XXH3_state_t st;
    uint64_t tex;
    int pal = psm == 0x13 || psm == 0x14;

    *maxAlpha = 255;
    sLookup.valid = 0;
    if (sCount == 0) {
        return -1;
    }
    if (psm == 0x13) { bw = 16; bh = 16; }
    else if (psm == 0x14) { bw = 32; bh = 16; }
    else if (psm == 0x00) { bw = 8; bh = 8; }
    else {
        if (getenv("BT3_TEX_LOG") != NULL) {
            fprintf(stderr, "texpack: unhashed %ux%u psm %02x\n", tw, th, psm);
        }
        return -1;
    }
    if (tw < bw || th < bh) {
        return -1; /* smaller than a block: hashed another way there, which is not worked out */
    }
    XXH3_64bits_reset(&st);
    for (y = 0; y < th; y += bh) {
        for (x = 0; x < tw; x += bw) {
            const uint8_t *p = Gs_BlockPtr(tbp, tbw, psm, x, y);
            XXH3_64bits_update(&st, p, 256);
            if (pal) Gs_IndexBounds(p, psm == 0x14, &lo, &hi);
        }
    }
    tex = XXH3_64bits_digest(&st);
    while ((1u << wl) < tw) { wl++; }
    while ((1u << hl) < th) { hl++; }
    bits = psm | wl << 6 | hl << 10 | (tcc & 1) << 14;
    if (!pal) {
        sLookup.tex = tex;
        sLookup.clutUsed = sLookup.clutAll = 0;
        sLookup.bits = bits;
        sLookup.pal = 0;
        sLookup.valid = 1;
        if (getenv("BT3_TEX_LOG") != NULL) {
            fprintf(stderr, "texpack: direct  %ux%u 32-bit %llx-%08x\n", tw, th, (unsigned long long)tex, bits);
        }
        return find_image(tex, 0, bits, 0);
    }
    if (cpsm != 0) {
        return -1; /* 16-bit palettes are expanded with TEXA there: not seen in this game */
    }
    {
        uint32_t clut[256], count = psm == 0x13 ? 256 : 16;
        int id;
        for (i = 0; i < count; i++) {
            /* index order: an 8-bit palette is stored with entries 8..15 and 16..23 of every 32 exchanged */
            uint32_t k = psm == 0x13 ? ((i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1)) : i;
            clut[i] = psm == 0x13 ? Gs_VramRead(cbp, 1, cpsm, k & 15, k >> 4) : Gs_VramRead(cbp, 1, cpsm, i & 7, i >> 3);
        }
        /* the largest alpha the ORIGINAL has (over the palette entries it uses): see the header of this function */
        *maxAlpha = 0;
        for (i = lo; i <= hi; i++) {
            if (clut[i] >> 24 > *maxAlpha) {
                *maxAlpha = clut[i] >> 24;
            }
        }
        id = find_image(tex, XXH3_64bits(&clut[lo], (hi - lo + 1) * 4), bits, 1);
        if (id < 0) {
            id = find_image(tex, XXH3_64bits(clut, count * 4), bits, 1);
        }
        sLookup.tex = tex;
        sLookup.clutUsed = XXH3_64bits(&clut[lo], (hi - lo + 1) * 4);
        sLookup.clutAll = XXH3_64bits(clut, count * 4);
        sLookup.bits = bits;
        sLookup.pal = 1;
        sLookup.valid = 1;
        if (getenv("BT3_TEX_LOG") != NULL) { /* which textures a pack has and which it has not, by the name it would need */
            fprintf(stderr, "texpack: %s %ux%u %u-bit %llx-%llx-%08x%s%s\n", id >= 0 ? "found  " : "missing", tw, th, psm == 0x13 ? 8u : 4u,
                    (unsigned long long)tex, (unsigned long long)XXH3_64bits(&clut[lo], (hi - lo + 1) * 4), bits, id >= 0 ? " " : "",
                    id >= 0 ? sEntries[id].path : "");
        }
        return id;
    }
}

/* A PNG: decoded by SDL, and its smaller copies (mip levels) made here, each the average of 2 x 2 pixels of the one
   before. */
static int load_png(const char *path,const void *source,size_t size,TexPackImage *img) {
    SDL_IOStream *io=source?SDL_IOFromConstMem(source,size):NULL;
    SDL_Surface *png = source?(io?SDL_LoadPNG_IO(io,true):NULL):SDL_LoadPNG(path), *rgba;
    uint32_t w, h, total = 0, lw, lh, at = 0, x, y, c;
    int level, levels = 0;
    uint8_t *f;

    if (png == NULL) {
        return 0;
    }
    rgba = SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(png);
    if (rgba == NULL || rgba->w <= 0 || rgba->h <= 0 || rgba->w > 8192 || rgba->h > 8192) {
        SDL_DestroySurface(rgba);
        return 0;
    }
    w = (uint32_t)rgba->w;
    h = (uint32_t)rgba->h;
    for (lw = w, lh = h; levels < TEXPACK_MAX_LEVELS; lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        total += lw * lh * 4;
        levels++;
        if (lw == 1 && lh == 1) {
            break;
        }
    }
    f = SDL_malloc(total);
    if (f == NULL) { SDL_DestroySurface(rgba); return 0; }
    for (y = 0; y < h; y++) {
        memcpy(f + y * w * 4, (const uint8_t *)rgba->pixels + y * (uint32_t)rgba->pitch, w * 4);
    }
    SDL_DestroySurface(rgba);
    img->file = f;
    img->w = w;
    img->h = h;
    img->format = 0;
    img->levels = levels;
    for (level = 0, lw = w, lh = h; level < levels; level++) {
        img->data[level] = f + at;
        img->bytes[level] = lw * lh * 4;
        if (level + 1 < levels) {
            const uint8_t *src = f + at;
            uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
            uint8_t *dst = f + at + lw * lh * 4;
            for (y = 0; y < nh; y++) {
                for (x = 0; x < nw; x++) {
                    uint32_t x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : lw - 1;
                    uint32_t y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : lh - 1;
                    for (c = 0; c < 4; c++) {
                        dst[(y * nw + x) * 4 + c] = (uint8_t)((src[(y0 * lw + x0) * 4 + c] + src[(y0 * lw + x1) * 4 + c] +
                                                               src[(y1 * lw + x0) * 4 + c] + src[(y1 * lw + x1) * 4 + c] + 2) / 4);
                    }
                }
            }
            at += lw * lh * 4;
            lw = nw;
            lh = nh;
        }
    }
    return 1;
}

static int load(const Entry *entry, TexPackImage *img) {
    const char *path=entry->path;void *zipped=NULL;
    size_t size = 0;
    uint8_t *f;
    uint32_t w, h, mips, flags, fourcc, bpp, blk, at = 128, level;

    memset(img, 0, sizeof(*img));
    if(entry->archive>=0){
        mz_zip_archive_file_stat st;
        mz_zip_archive *zip=&sZip[entry->archive].zip;
        if(!mz_zip_reader_file_stat(zip,entry->member,&st)||st.m_uncomp_size>(128u<<20))return 0;
        size=(size_t)st.m_uncomp_size;zipped=SDL_malloc(size);if(!zipped)return 0;
        if(!mz_zip_reader_extract_to_mem(zip,entry->member,zipped,size,0)){SDL_free(zipped);return 0;}
    }
    if (strlen(path) > 4 && SDL_strcasecmp(path + strlen(path) - 4, ".png") == 0) {
        int ok=load_png(path,zipped,size,img);SDL_free(zipped);return ok;
    }
    f = zipped?zipped:SDL_LoadFile(path, &size);
    if (f == NULL || size < 128 || memcmp(f, "DDS ", 4) != 0) {
        SDL_free(f);
        return 0;
    }
    memcpy(&h, f + 12, 4);
    memcpy(&w, f + 16, 4);
    memcpy(&mips, f + 28, 4);
    memcpy(&flags, f + 80, 4);
    memcpy(&fourcc, f + 84, 4);
    memcpy(&bpp, f + 88, 4);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        SDL_free(f);
        return 0;
    }
    if (mips == 0) {
        mips = 1;
    }
    if ((flags & 4) && fourcc == 0x31545844) { img->format = 1; blk = 8; }        /* DXT1 */
    else if ((flags & 4) && fourcc == 0x33545844) { img->format = 2; blk = 16; }  /* DXT3 */
    else if ((flags & 4) && fourcc == 0x35545844) { img->format = 3; blk = 16; }  /* DXT5 */
    else if (!(flags & 4) && bpp == 32) { img->format = 0; blk = 0; }
    else {
        SDL_free(f);
        return 0;
    }
    img->file = f;
    img->w = w;
    img->h = h;
    for (level = 0; level < mips && level < TEXPACK_MAX_LEVELS; level++) {
        uint32_t lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1;
        uint32_t bytes = blk ? ((lw + 3) / 4) * ((lh + 3) / 4) * blk : lw * lh * 4;
        if (at + bytes > size) {
            break;
        }
        if (blk == 0) { /* plain pixels: B, G, R, A in the file unless the red mask says otherwise */
            uint32_t rmask, i;
            memcpy(&rmask, f + 92, 4);
            if (rmask != 0x000000FF) {
                for (i = 0; i < lw * lh; i++) {
                    uint8_t t = f[at + i * 4];
                    f[at + i * 4] = f[at + i * 4 + 2];
                    f[at + i * 4 + 2] = t;
                }
            }
        }
        img->data[level] = f + at;
        img->bytes[level] = bytes;
        at += bytes;
        if (lw == 1 && lh == 1) {
            level++;
            break;
        }
    }
    img->levels = (int)level;
    if (img->levels == 0) {
        SDL_free(f);
        img->file = NULL;
        return 0;
    }
    return 1;
}

static void image_free(TexPackImage *img) {
    SDL_free(img->file);
    img->file = NULL;
}

int TexPack_AnimeFind(int *ids, int max, int *other) {
    int n = 0, pass, tcc;
    uint32_t i;

    *other = 0;
    if (!sLookup.valid) {
        return 0;
    }
    /* pass 0: this very texture; pass 1 (none found): the same one under another palette (the game repaints a fighter's
       palettes for a moment, a hit), one from each folder. And as find_image does for the plain replacements (BT4):
       none found with the texture's own TCC bit, the same picture named with the other one (tcc 1). */
    for (tcc = 0; tcc < 2 && n == 0; tcc++)
    for (pass = 0; pass < 2 && n == 0; pass++) {
        uint32_t want = tcc ? sLookup.bits ^ 0x4000u : sLookup.bits;
        for (i = 0; i < sAnimeCount && n < max; i++) {
            const Entry *e = &sEntries[sAnimeIds[i]];
            int k, seen = 0;
            if (e->tex != sLookup.tex || e->bits != want || e->pal != sLookup.pal) {
                continue;
            }
            if (pass == 0 && e->pal && e->clut != sLookup.clutUsed && e->clut != sLookup.clutAll) {
                continue;
            }
            for (k = 0; k < n; k++) {
                seen |= sEntries[ids[k]].folder == e->folder;
            }
            if (!seen) {
                ids[n++] = (int)sAnimeIds[i];
            }
        }
        *other = pass == 1 && n > 0;
    }
    return n;
}

int TexPack_AnimeSiblings(int id, int *ids, int max) {
    int n = 0;
    uint32_t i;
    if (id < 0 || (uint32_t)id >= sCount || !sEntries[id].anime) {
        return 0;
    }
    for (i = 0; i < sAnimeCount && n < max; i++) {
        const Entry *e = &sEntries[sAnimeIds[i]], *o = &sEntries[id];
        if (e->tex == o->tex && e->bits == o->bits && e->pal == o->pal && e->folder == o->folder) {
            ids[n++] = (int)sAnimeIds[i];
        }
    }
    return n;
}

uint64_t TexPack_AnimeFoldersMarking(uint64_t tex) {
    uint64_t mask = 0;
    int f, k;
    for (f = 0; f < sFolderCount && f < 64; f++) {
        for (k = 0; k < sFolders[f].markerCount; k++) {
            if (sFolders[f].markers[k] == tex) {
                mask |= 1ull << f;
                break;
            }
        }
    }
    return mask;
}

int TexPack_AnimeFolderEntries(int folder, int *ids, int max) {
    int n = 0;
    uint32_t i;
    for (i = 0; i < sAnimeCount && n < max; i++) {
        if (sEntries[sAnimeIds[i]].folder == folder) {
            ids[n++] = (int)sAnimeIds[i];
        }
    }
    return n;
}

uint64_t TexPack_LastTex(void) {
    return sLookup.valid ? sLookup.tex : 0;
}

int TexPack_AnimeFolder(int id) {
    return id >= 0 && (uint32_t)id < sCount && sEntries[id].anime ? sEntries[id].folder : -1;
}

/* Whether a folder's colours are for the fighter whose model is file `model` (-1 = not known): yes without an
   anime_models.txt; with one, only for the models listed (a shared piece of another fighter takes nothing). */
int TexPack_AnimeFolderAllows(int folder, int model) {
    int k;
    if (folder < 0 || folder >= sFolderCount || !sFolders[folder].hasModels || model < 0) {
        return 1; /* (a model not known: as without the list, rather than a fighter without its colours) */
    }
    for (k = 0; k < sFolders[folder].modelCount; k++) {
        if (sFolders[folder].models[k] == model) {
            return 1;
        }
    }
    return 0;
}

/* 1 = some folder has an anime_models.txt (the decoded textures then depend on which models the fighters have) */
int TexPack_AnimeAnyModels(void) {
    int f;
    for (f = 0; f < sFolderCount; f++) {
        if (sFolders[f].hasModels) {
            return 1;
        }
    }
    return 0;
}

/* 1 = there are anime folders: the game's code then keeps each fighter's own shading ramps where the BT4 swaps in
   the light file's common one (src/sys/gfxm_d_c.c, flag 0x40000), as the BT3 does */
int Port_AnimeOwnRamps(void) {
    return sFolderCount > 0;
}

int TexPack_AnimeFolderIsForm(int folder) {
    return folder >= 0 && folder < sFolderCount && sFolders[folder].form;
}

int TexPack_AnimeIsHair(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].hairCount; k++) {
        if (sFolders[folder].hair[k] == sEntries[id].tex) {
            return 1;
        }
    }
    return 0;
}

int TexPack_AnimeBrillo(int id) {
    int folder = TexPack_AnimeFolder(id);
    return folder >= 0 ? sFolders[folder].brillo : 0;
}

int TexPack_AnimeView(int id) {
    int folder = TexPack_AnimeFolder(id);
    return folder >= 0 ? sFolders[folder].view : 0;
}

int TexPack_AnimeUpStrip(int id, uint32_t uv) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].upCount; k++) {
        if ((uint32_t)sFolders[folder].up[k] == uv) {
            return 1;
        }
    }
    return 0;
}

int TexPack_AnimeIsGloss(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].glossCount; k++) {
        if (sFolders[folder].gloss[k] == sEntries[id].tex) {
            return 1;
        }
    }
    return 0;
}

int TexPack_GlossAny(uint64_t tex) {
    int f, k;
    for (f = 0; f < sFolderCount; f++) {
        for (k = 0; k < sFolders[f].glossCount; k++) {
            if (sFolders[f].gloss[k] == tex) {
                return 1;
            }
        }
        /* (tenk2) an anime_hide.txt's too: its strips hidden by name when its own anime colours are not on it */
        for (k = 0; k < sFolders[f].hideCount; k++) {
            if (sFolders[f].hide[k] == tex) {
                return 1;
            }
        }
    }
    return 0;
}

/* (tenk2, 14-nov-2026) a texture an anime_hide.txt of this fighter's folders lists (by name, without its anime colours:
   Cell's metallic layer 2ea86cd1 was drawn over the body in every build since Rev21) */
int TexPack_HideFor(uint64_t tex, int model) {
    int f, k;
    if (model < 0) {
        return 0;
    }
    for (f = 0; f < sFolderCount; f++) {
        if (!sFolders[f].hasModels || !TexPack_AnimeFolderAllows(f, model)) {
            continue;
        }
        for (k = 0; k < sFolders[f].hideCount; k++) {
            if (sFolders[f].hide[k] == tex) {
                return 1;
            }
        }
    }
    return 0;
}

int TexPack_GlossFor(uint64_t tex, int model) {
    int f, k;
    if (model < 0) {
        return 0;
    }
    for (f = 0; f < sFolderCount; f++) {
        if (!sFolders[f].hasModels || !TexPack_AnimeFolderAllows(f, model)) {
            continue;
        }
        for (k = 0; k < sFolders[f].glossCount; k++) {
            if (sFolders[f].gloss[k] == tex) {
                return 1;
            }
        }
    }
    return 0;
}

float TexPack_AnimeEdge(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return -1.0f;
    }
    for (k = 0; k < sFolders[folder].edgeCount; k++) {
        if (sFolders[folder].edgeTex[k] == sEntries[id].tex) {
            return sFolders[folder].edgeAt[k];
        }
    }
    return -1.0f;
}

int TexPack_AnimeCapas(int id, int w[3]) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].capCount; k++) {
        if (sFolders[folder].capTex[k] == sEntries[id].tex) {
            w[0] = sFolders[folder].capW[k][0];
            w[1] = sFolders[folder].capW[k][1];
            w[2] = sFolders[folder].capW[k][2];
            return sFolders[folder].capN[k];
        }
    }
    return 0;
}

int TexPack_AnimeEsfera(int id, uint32_t *osc, uint32_t *clr) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].esfCount; k++) {
        if (sFolders[folder].esfTex[k] == sEntries[id].tex) {
            *osc = sFolders[folder].esfOsc[k];
            *clr = sFolders[folder].esfClr[k];
            return 1;
        }
    }
    return 0;
}

int TexPack_AnimeLuz(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].luzCount; k++) {
        if (sFolders[folder].luz[k] == sEntries[id].tex) {
            return 1;
        }
    }
    return 0;
}

int TexPack_AnimeIsHidden(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].hideCount; k++) {
        if (sFolders[folder].hide[k] == sEntries[id].tex) {
            return 1;
        }
    }
    return 0;
}

int TexPack_AnimeIsMarker(int id) {
    int folder = TexPack_AnimeFolder(id), k;
    if (folder < 0) {
        return 0;
    }
    for (k = 0; k < sFolders[folder].markerCount; k++) {
        if (sFolders[folder].markers[k] == sEntries[id].tex) {
            return 1;
        }
    }
    return 0;
}

/* Read at once (they are few and small), and not kept: the caller makes its texture and lets go (TexPack_Free).
   *cut: the number in "anime_cut.txt" in the same folder (-100..100), 0 without one. */
int TexPack_LoadAnime(int id, TexPackImage *img, int *cut) {
    memset(img, 0, sizeof(*img));
    *cut = 0;
    if (id < 0 || (uint32_t)id >= sCount || !sEntries[id].anime) {
        return 0;
    }
    *cut = sFolders[sEntries[id].folder].cut;
    return load(&sEntries[id], img); /* (BT4's load takes the entry: a file may be in a ZIP pack) */
}

int TexPack_AnimeMid(int id) {
    int folder = TexPack_AnimeFolder(id);
    return folder >= 0 ? sFolders[folder].mid : 30;
}

void TexPack_Free(TexPackImage *img) {
    image_free(img);
}

static size_t image_bytes(const TexPackImage *img) {
    size_t n = 0;
    int level;
    for (level = 0; level < img->levels; level++) {
        n += img->bytes[level];
    }
    return n;
}

/* Called with sLock held: a file read, into its entry */
static void store(uint32_t id, int ok, const TexPackImage *img) {
    Entry *e = &sEntries[id];
    if (!ok) {
        e->state = 3;
        return;
    }
    e->img = *img;
    e->held = image_bytes(img);
    e->state = 2;
    sHeld[sHeldCount++] = id;
    sHeldBytes += e->held;
}

static int SDLCALL reader(void *unused) {
    (void)unused;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        uint32_t id;
        TexPackImage img;
        int ok;
        SDL_LockMutex(sLock);
        /* Only the renderer may evict borrowed image data. Park the reader
           after one image crosses the budget, including while no frames are
           consumed in a hidden/paused window. */
        while (sHead == sTail || sHeldBytes > sBudget) {
            SDL_WaitCondition(sWake, sLock);
        }
        id = sQueue[sTail++ & (QUEUE - 1)];
        SDL_UnlockMutex(sLock);
        ok = load(&sEntries[id], &img);
        SDL_LockMutex(sLock);
        store(id, ok, &img);
        SDL_UnlockMutex(sLock);
    }
    return 0;
}

/* Over the budget: the files asked for longest ago leave memory, down to three quarters of it (not `keep`, the one
   just asked for). Only this thread frees them; the reader only adds. Called with sLock held. */
static void trim(uint32_t keep) {
    /* Hits inside the full budget keep the warm working set. Only crossing the
       high watermark starts a sweep to the lower one; otherwise every hit
       above 75% evicted usable artwork and queued another disk/decode cycle. */
    if (sHeldBytes <= sBudget) return;
    while (sHeldBytes > sBudget / 4 * 3 && sHeldCount > 0) {
        uint32_t i, old = sHeldCount;
        for (i = 0; i < sHeldCount; i++) {
            if (sHeld[i] != keep && (old == sHeldCount || sEntries[sHeld[i]].used < sEntries[sHeld[old]].used)) {
                old = i;
            }
        }
        if (old == sHeldCount) {
            return;
        }
        {
            Entry *e = &sEntries[sHeld[old]];
            image_free(&e->img);
            sHeldBytes -= e->held;
            e->held = 0;
            e->state = 0;
            sHeld[old] = sHeld[--sHeldCount];
        }
    }
    if (sHeldBytes <= sBudget && sWake != NULL) SDL_SignalCondition(sWake);
}

int TexPack_Get(int id, TexPackImage *img) {
    Entry *e;
    int r;

    if (id < 0 || (uint32_t)id >= sCount) {
        return -1;
    }
    if (sSync < 0) {
        const char *mb = getenv("BT3_PACK_MB");
        sSync = getenv("BT3_PACK_SYNC") != NULL && atoi(getenv("BT3_PACK_SYNC")) != 0;
        sBudget = mb != NULL && atoi(mb) > 0 ? (size_t)atoi(mb) << 20 : TEXPACK_BUDGET;
        sHeld = malloc(sCount * sizeof(uint32_t));
        sLock = SDL_CreateMutex();
        sWake = SDL_CreateCondition();
        if (!sSync && sLock != NULL && sWake != NULL) {
            sReader = SDL_CreateThread(reader, "bt3-texpack", NULL);
        }
        if (sReader == NULL) {
            sSync = 1;
        }
    }
    e = &sEntries[id];
    SDL_LockMutex(sLock);
    /* Pending requests must also let a budget-blocked reader make progress.
       Keep the requested image; no other TexPack_Get borrow is live here. */
    trim((uint32_t)id);
    e->used = ++sTick;
    if (e->state == 0 && sSync) {
        TexPackImage read;
        int ok = load(e, &read);
        store((uint32_t)id, ok, &read);
    } else if (e->state == 0 && sHead - sTail < QUEUE) {
        sQueue[sHead++ & (QUEUE - 1)] = (uint32_t)id;
        e->state = 1;
        SDL_SignalCondition(sWake);
    }
    r = e->state == 2 ? 1 : e->state == 3 ? -1 : 0;
    if (r == 1) {
        *img = e->img;
        trim((uint32_t)id);
    }
    SDL_UnlockMutex(sLock);
    return r;
}
