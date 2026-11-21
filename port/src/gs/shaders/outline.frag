#version 450
// Native version of the game's outline effect (ObjOutline_Draw, src/sys/gfxm_a.c).
// Every model writes an object number into the frame's alpha; the PS2 pass turns that number into a value with
// a 256-entry table, subtracts the same image shifted by one pixel down and one pixel right (so only the places
// where the number changes survive), and blends a dark rectangle (0x64 per channel) through the result.
// Here: the same table and the same two neighbours, read from the exact copy of the alpha byte, and the result
// is the amount to SUBTRACT from the picture (the pipeline's blend is destination minus source).
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0) uniform sampler2D aux; // .r = the GS alpha byte / 255
#ifndef NO_CAP
layout(set = 2, binding = 1) uniform sampler2D depthTex; // (tenk26) the target's depth (grows toward the camera)
#endif
layout(set = 3, binding = 0) uniform Params {
    vec4 step; // x: the line's width across in uv, y: one PS2 pixel down in uv, z: darkness (0x64 / 255)
#ifndef NO_CAP
    vec4 cap[5]; // (Rev24) for each fighter (its row of the alpha table: id / 15), the share of the width it is drawn at
                 // (gs_draw.c GsDraw_OutlineCaps: no wider than the lines inside, anime_lineas.txt); 0 = as it is
                 // (tenk26) cap[4].xyz: object numbers + 1 whose outline also follows the depth (0 = none), w: the jump's threshold
#endif
} p;
#ifndef NO_CAP
// (tenk22, 20-nov-2026) the object numbers of the thin pieces (gs_draw.c anime_contorno_fino.txt: Syn/Omega's whiskers) and how
// many times as wide their line is drawn: cap[1..3] = 6 pairs <number> <times>, -10 = none
float thinTimes(vec2 uv) {
    int id = int(texture(aux, uv).r * 255.0 + 0.5);
    float t = 1.0;
    if (id == int(floor(p.cap[1].x + 0.5))) t = p.cap[1].y;
    if (id == int(floor(p.cap[1].z + 0.5))) t = p.cap[1].w;
    if (id == int(floor(p.cap[2].x + 0.5))) t = p.cap[2].y;
    if (id == int(floor(p.cap[2].z + 0.5))) t = p.cap[2].w;
    if (id == int(floor(p.cap[3].x + 0.5))) t = p.cap[3].y;
    if (id == int(floor(p.cap[3].z + 0.5))) t = p.cap[3].w;
    return t;
}
// (tenk26, 21-nov-2026) Inside ONE object (Vegeta SSJ destroyed: his whole hair one texture, one palette, one number) the
// line between a lock and the one behind it: where the depth jumps between two pixels of the same number, more than the
// surface's own slope would give, the nearer one is the edge of the lock in front. Only for the numbers in cap[4]
// (gs_draw.c anime_contorno_profundidad.txt, with their models): every other piece as before.
int rawId(vec2 uv) { return int(texture(aux, uv).r * 255.0 + 0.5); }
bool depthPiece(int id) {
    return id + 1 == int(floor(p.cap[4].x + 0.5)) || id + 1 == int(floor(p.cap[4].y + 0.5)) || id + 1 == int(floor(p.cap[4].z + 0.5));
}
// a jump from a (nearer) to b (one pixel further along d), a's slope measured on the pixel before it
bool depthJump(vec2 a, vec2 d, int id) {
    vec2 b = a + d;
    if (rawId(b) != id || rawId(a) != id) return false;
    float za = texture(depthTex, a).r, zb = texture(depthTex, b).r, zp = texture(depthTex, a - d).r;
    float slope = abs(za - zp), jump = za - zb; // (the depth grows toward the camera: a nearer means za > zb)
    float rel = p.cap[4].w > 0.0 ? p.cap[4].w : 0.0015; // (anime_contorno_profundidad.txt 'umbral <v>')
    return jump > 4.0 * slope + rel * za + 1e-6;
}
// coverage of this pixel by such a line W pixels wide, drawn on the nearer side of the jump
float depthLine(vec2 uv, vec2 tx, vec2 ty, float W) {
    int id = rawId(uv);
    if (id == 255 || !depthPiece(id)) return 0.0;
    float cov = 0.0;
    vec2 dirs[4] = vec2[4](tx, -tx, ty, -ty);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            if (float(j) > W) break;
            vec2 a = uv + float(j) * dirs[i];
            if (rawId(a) != id) break;
            if (depthJump(a, dirs[i], id)) {
                cov = max(cov, clamp(W - float(j), 0.0, 1.0));
                break;
            }
        }
    }
    return cov;
}
#endif
float table(vec2 uv) {
    int id = int(texture(aux, uv).r * 255.0 + 0.5);
    if (id == 255) return 0.0;           // "nothing" (stage, sky): the table gives it no colour, so an object next to it has an edge
    int c = (id << 3) & 0xFF;            // ObjOutline_BuildClut
    return float(c == 0 ? 0x80 : c);
}
void main() {
    // Widescreen stretches the PS2's 512 pixels across the wider picture, so a line one PS2 pixel wide grew with the
    // aspect ratio on the left and right of every shape. Across, the width (step.x) is narrowed by the stretch and
    // measured in texels of the image: a texel at distance d from the edge darkens by clamp(width - d + 1, 0, 1),
    // which gives lines of the same thickness at any shape (a fraction of a texel as a lighter last texel).
    float tx = 1.0 / float(textureSize(aux, 0).x), width = p.step.x / tx;
    float here = table(vUv);
    // Nothing to subtract (the pipeline: picture minus this, colour only), so nothing is written: the same picture,
    // without reading and writing back every pixel of the screen. A place with no object (the table's 0: stage,
    // sky, most of the picture) is never on the larger side of an edge, so its neighbours are not even read.
    float amount = 0.0;
#ifdef NO_CAP
    if (here == 0.0 && p.step.w == 0.0) {
        discard;
    }
    float dy = p.step.y;
    float down = table(vUv + vec2(0.0, dy)), up = table(vUv - vec2(0.0, dy));
    // the subtraction keeps only positive differences: the line is drawn on the side with the larger value
    amount = here > down || here > up ? 1.0 : 0.0;
    for (int k = 1; k <= 8 && float(k) < width + 1.0; k++) {
        if (here > table(vUv + vec2(float(k) * tx, 0.0)) || here > table(vUv - vec2(float(k) * tx, 0.0))) {
            amount = max(amount, clamp(width - float(k) + 1.0, 0.0, 1.0));
            break; // (the nearest edge gives the most)
        }
    }
#else
    // (Rev30, 13-nov-2026) Black and whole, soft only on its outer side, as wide at any distance (the user: Rev29's outline
    // came out in dots, greyish, a dot of the hair's colour left past it at the tip of each lock, and far away thinner
    // than the lines inside). The line is drawn on the side with the larger value, as the game does: every pixel there with
    // a smaller one beside it (left, right, up, down) is the line, whole; further in, as much of the pixel as lies within
    // the width from the edge (the edge a straight one through the 5 x 5 round it: of those, the share F on the smaller
    // side is (2.5 - d) / 5, d the distance from the edge to this pixel's middle). And just outside, a pixel with a larger
    // one round it is darkened by how much of it the line, run straight, would cover (a corner of the stair: up to half):
    // the stair's steps filled in, a smooth line (no saw teeth). The cap of Rev24 (thinner far away) is no more.
    vec2 ty = vec2(0.0, 1.0 / float(textureSize(aux, 0).y)), txv = vec2(tx, 0.0);
    float l = table(vUv - txv), r = table(vUv + txv), u = table(vUv - ty), dn = table(vUv + ty);
    float ul = table(vUv - txv - ty), ur = table(vUv + txv - ty), dl = table(vUv - txv + ty), dr = table(vUv + txv + ty);
    bool edge = l < here || r < here || u < here || dn < here;
    float hi = max(max(max(l, r), max(u, dn)), max(max(ul, ur), max(dl, dr)));
    bool inner = here > 0.0 && (edge || ul < here || ur < here || dl < here || dr < here ||
                 table(vUv + 2.0 * txv) < here || table(vUv - 2.0 * txv) < here || table(vUv + 2.0 * ty) < here || table(vUv - 2.0 * ty) < here);
    // (tenk26) inside a piece whose outline follows the depth: a line where a lock is in front of another
    float dline = p.cap[0].x > 0.0 ? depthLine(vUv, txv, ty, p.cap[0].x) : 0.0;
    if (!inner && hi <= here && p.step.w == 0.0 && dline <= 0.0) {
        discard; // (nothing larger round it, nor smaller: no edge near; most of the picture)
    }
    if (p.cap[0].x > 0.0 && p.step.w == 0.0) {
        // (tenk2, 14-nov-2026) as the lines inside the textures (gs.frag mode 4): a line W pixels wide (cap[0].x) on the larger
        // side of the edge, black, coverage 1 - smoothstep(hh - 0.5, hh + 0.5, |s - hh|) with hh = W / 2 and s the distance
        // from the edge (into the larger side; negative outside it), the edge a straight one through the 5 x 5 round it
        float hh = 0.5 * p.cap[0].x, cov = 0.0;
        // (tenk22) a thin piece's line narrower: inside it (this pixel's number), and outside a thin piece next to it
        float hIn = hh * thinTimes(vUv);
        float hOut = hh * min(min(thinTimes(vUv - txv), thinTimes(vUv + txv)), min(thinTimes(vUv - ty), thinTimes(vUv + ty)));
        if (inner) {
            hh = hIn;
            float other = 0.0;
            for (int j = -2; j <= 2; j++) {
                for (int i = -2; i <= 2; i++) {
                    other += table(vUv + float(i) * txv + float(j) * ty) < here ? 1.0 : 0.0;
                }
            }
            float s = 2.5 - 5.0 * (other / 25.0);
            if (edge) {
                // a pixel of the figure with a smaller one beside it is on the line, whatever the 5 x 5 says: at the tip of a
                // lock (thin, mostly outside) the share put it outside and the hair's colour was left there as a dot
                s = clamp(s, min(0.5, hh), max(0.5, hh));
            }
            cov = max(cov, 1.0 - smoothstep(hh - 0.5, hh + 0.5, abs(s - hh)));
        }
        if (hi > here) {
            hh = hOut;
            float larger = 0.0;
            for (int j = -2; j <= 2; j++) {
                for (int i = -2; i <= 2; i++) {
                    larger += table(vUv + float(i) * txv + float(j) * ty) > here ? 1.0 : 0.0;
                }
            }
            float s = -(2.5 - 5.0 * (larger / 25.0));
            cov = max(cov, 1.0 - smoothstep(hh - 0.5, hh + 0.5, abs(s - hh)));
        }
        cov = max(cov, dline); /* (tenk26) */
        if (cov <= 0.0) {
            discard;
        }
        outColor = vec4(vec3(cov), 0.0);
        return;
    }
    if (edge) {
        amount = 1.0;
    } else if (inner) {
        float other = 0.0;
        for (int j = -2; j <= 2; j++) {
            for (int i = -2; i <= 2; i++) {
                other += table(vUv + float(i) * txv + float(j) * ty) < here ? 1.0 : 0.0;
            }
        }
        float d = 2.5 - 5.0 * (other / 25.0);
        amount = clamp(min(d + 0.5, width) - max(d - 0.5, 0.0), 0.0, 1.0);
    }
    if (hi > here) {
        // outside a larger one: the share of the 5 x 5 round it larger than this, the distance out from that edge
        float larger = 0.0;
        for (int j = -2; j <= 2; j++) {
            for (int i = -2; i <= 2; i++) {
                larger += table(vUv + float(i) * txv + float(j) * ty) > here ? 1.0 : 0.0;
            }
        }
        float e = 2.5 - 5.0 * (larger / 25.0);
        amount = max(amount, clamp(0.5 - e, 0.0, 0.5));
    }
#endif
    if (amount == 0.0 && p.step.w == 0.0) {
        discard;
    }
    if (p.step.w != 0.0) { // debug view: darken by the object number instead (255 = "nothing" stays untouched)
        float id = texture(aux, vUv).r * 255.0;
        outColor = vec4(id > 254.5 ? vec3(0.0) : vec3(0.3 + id / 16.0, id == 0.0 ? 0.6 : 0.0, 0.0), 0.0);
        return;
    }
    outColor = vec4(vec3(p.step.z * amount), 0.0);
}
