#!/usr/bin/env python3
"""16x16 blue-noise threshold matrix (void-and-cluster, Ulichney 1993) for
the mode 13h dithering in platform/dos_ovl/video.c. Prints the C table.
Deterministic: fixed seed, so the table in the source can be regenerated."""
import math
import random

N, SIGMA = 16, 1.5


def energy(pat):
    e = [[0.0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            if pat[y][x]:
                for v in range(N):
                    for u in range(N):
                        dx = min(abs(u - x), N - abs(u - x))
                        dy = min(abs(v - y), N - abs(v - y))
                        e[v][u] += math.exp(-(dx * dx + dy * dy) / (2 * SIGMA * SIGMA))
    return e


def extreme(pat, want, cmp):
    e = energy(pat)
    best = None
    for y in range(N):
        for x in range(N):
            if pat[y][x] == want and (best is None or cmp(e[y][x], best[0])):
                best = (e[y][x], x, y)
    return best[1], best[2]


def main():
    random.seed(866)
    pat = [[0] * N for _ in range(N)]
    ones = N * N // 10
    for i in random.sample(range(N * N), ones):
        pat[i // N][i % N] = 1
    # relax the initial pattern: move the tightest cluster into the largest void
    while True:
        cx, cy = extreme(pat, 1, lambda a, b: a > b)
        pat[cy][cx] = 0
        vx, vy = extreme(pat, 0, lambda a, b: a < b)
        if (vx, vy) == (cx, cy):
            pat[cy][cx] = 1
            break
        pat[vy][vx] = 1
    rank = [[0] * N for _ in range(N)]
    proto = [row[:] for row in pat]
    for r in range(ones - 1, -1, -1):
        cx, cy = extreme(pat, 1, lambda a, b: a > b)
        pat[cy][cx] = 0
        rank[cy][cx] = r
    pat = proto
    for r in range(ones, N * N):
        vx, vy = extreme(pat, 0, lambda a, b: a < b)
        pat[vy][vx] = 1
        rank[vy][vx] = r
    print("static const uint8_t g_blue16[16][16] = {")
    for y in range(N):
        print("    { " + ", ".join("%3d" % rank[y][x] for x in range(N)) + " },")
    print("};")


main()
