#!/usr/bin/env python3
"""
Asteroid belt density field prototype.
Generates noise-based density maps showing rivers, lakes, and oceans of rock.
"""
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import sys, os

# --- Simplex-like noise via permutation hash (no external deps) ---

def _fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)

def _lerp(a, b, t):
    return a + t * (b - a)

def _grad2(h, x, y):
    """Gradient from hash, 2D. Vectorized."""
    h = h & 7
    u = np.where(h < 4, x, y)
    v = np.where(h < 4, y, x)
    return np.where(h & 1, -u, u) + np.where(h & 2, -v, v)

class PerlinNoise2D:
    def __init__(self, seed=0):
        rng = np.random.RandomState(seed)
        self.perm = np.arange(256, dtype=int)
        rng.shuffle(self.perm)
        self.perm = np.tile(self.perm, 2)

    def __call__(self, x, y):
        """Evaluate noise at (x, y). Works with numpy arrays."""
        X = np.floor(x).astype(int) & 255
        Y = np.floor(y).astype(int) & 255
        xf = x - np.floor(x)
        yf = y - np.floor(y)
        u = _fade(xf)
        v = _fade(yf)
        aa = self.perm[self.perm[X] + Y]
        ab = self.perm[self.perm[X] + Y + 1]
        ba = self.perm[self.perm[X + 1] + Y]
        bb = self.perm[self.perm[X + 1] + Y + 1]
        x1 = _lerp(_grad2(aa, xf, yf), _grad2(ba, xf - 1, yf), u)
        x2 = _lerp(_grad2(ab, xf, yf - 1), _grad2(bb, xf - 1, yf - 1), u)
        return _lerp(x1, x2, v)


def asteroid_density(x, y, seed=432, world_scale=50000.0):
    """
    Multi-octave noise density field for asteroid belts.
    Returns 0.0 (empty space) to 1.0 (dense ocean of rock).

    Key idea: layer multiple noise scales to get:
    - Large-scale belt/gap structure (rivers and dry land)
    - Medium-scale pocket variation (lakes and pools)
    - Fine-scale graininess (individual cluster variation)
    """
    noise = PerlinNoise2D(seed)
    noise2 = PerlinNoise2D(seed + 7919)  # different pattern for ridged noise
    noise3 = PerlinNoise2D(seed + 104729)  # ore bias

    # Normalize coordinates to noise space
    nx = x / world_scale
    ny = y / world_scale

    # Octave 1: Large belt structure (rivers)
    # Use ridged noise for sharper belt edges
    n1 = noise(nx * 3.0, ny * 3.0)
    ridge = 1.0 - np.abs(n1)  # ridged: peaks become sharp ridges = belt centers
    ridge = ridge ** 2  # sharpen the ridges

    # Octave 2: Medium variation (lakes, pools)
    n2 = noise2(nx * 7.0, ny * 7.0)
    pools = np.clip((n2 + 0.3) * 0.8, 0, 1)  # biased positive for more filled areas

    # Octave 3: Fine detail (cluster graininess)
    n3 = noise(nx * 18.0, ny * 18.0)
    grain = n3 * 0.15

    # Octave 4: Very large scale bias (ocean regions vs desert regions)
    n4 = noise2(nx * 1.2, ny * 1.2)
    region_bias = np.clip((n4 + 0.2) * 0.7, 0, 1)

    # Combine: ridged belts modulated by regional bias and pools
    density = ridge * 0.6 + pools * 0.3 + grain
    density = density * (0.4 + region_bias * 0.6)  # regional modulation

    # Apply threshold curve: push low values to zero (create empty lanes)
    density = np.clip(density, 0, 1)
    density = np.where(density < 0.15, 0, (density - 0.15) / 0.85)  # hard floor
    density = density ** 0.7  # slight boost to mid-range

    return np.clip(density, 0, 1)


def ore_bias(x, y, seed=432, world_scale=50000.0):
    """
    Returns ore type bias as (ferrite, cuprite, crystal) weights.
    Different noise layers for each ore type create geographic ore zones.
    """
    noise_fe = PerlinNoise2D(seed + 31337)
    noise_cu = PerlinNoise2D(seed + 65537)
    noise_cr = PerlinNoise2D(seed + 99991)

    nx = x / world_scale
    ny = y / world_scale

    fe = np.clip(noise_fe(nx * 4.0, ny * 4.0) + 0.5, 0.1, 1.0)
    cu = np.clip(noise_cu(nx * 4.0, ny * 4.0) + 0.5, 0.1, 1.0)
    cr = np.clip(noise_cr(nx * 4.0, ny * 4.0) + 0.5, 0.1, 1.0)

    total = fe + cu + cr
    return fe / total, cu / total, cr / total


def render_density_map(seed, filename, title, world_scale=50000.0):
    """Render a single density map."""
    res = 800
    extent = world_scale * 1.2
    x = np.linspace(-extent, extent, res)
    y = np.linspace(-extent, extent, res)
    X, Y = np.meshgrid(x, y)

    D = asteroid_density(X, Y, seed=seed, world_scale=world_scale)

    fig, ax = plt.subplots(1, 1, figsize=(10, 10), facecolor='#080808')
    ax.set_facecolor('#080808')

    # Custom colormap: black -> dark blue -> teal -> bright green/white
    colors = ['#080808', '#0a1628', '#0d2847', '#12405a',
              '#1a6b5a', '#2a9060', '#5cc070', '#a0e080', '#e0f0c0']
    cmap = mcolors.LinearSegmentedColormap.from_list('belt', colors, N=256)

    ax.imshow(D, extent=[-extent, extent, -extent, extent],
              origin='lower', cmap=cmap, vmin=0, vmax=0.8, interpolation='bilinear')

    # Mark station positions
    stations = [(0, -2400, 'Refinery', 18000),
                (-3200, 2300, 'Yard', 15000),
                (3200, 2300, 'Beamworks', 15000)]
    for sx, sy, name, sr in stations:
        circle = plt.Circle((sx, sy), sr, fill=False, color='#3af0c0', linewidth=0.5, alpha=0.4)
        ax.add_patch(circle)
        ax.plot(sx, sy, 'o', color='#3af0c0', markersize=4)
        ax.text(sx + 800, sy + 800, name, color='#3af0c0', fontsize=7, alpha=0.7)

    ax.set_xlim(-extent, extent)
    ax.set_ylim(-extent, extent)
    ax.set_aspect('equal')
    ax.set_title(title, color='#c0d8f0', fontsize=14, pad=10)
    ax.tick_params(colors='#405060', labelsize=7)
    for spine in ax.spines.values():
        spine.set_color('#1a2a3a')

    plt.tight_layout()
    plt.savefig(filename, dpi=150, facecolor='#080808', bbox_inches='tight')
    plt.close()
    print(f"  saved {filename}")


def render_ore_map(seed, filename, title, world_scale=50000.0):
    """Render ore type distribution as RGB."""
    res = 800
    extent = world_scale * 1.2
    x = np.linspace(-extent, extent, res)
    y = np.linspace(-extent, extent, res)
    X, Y = np.meshgrid(x, y)

    D = asteroid_density(X, Y, seed=seed, world_scale=world_scale)
    fe, cu, cr = ore_bias(X, Y, seed=seed, world_scale=world_scale)

    # RGB: ferrite=orange, cuprite=blue, crystal=green
    R = D * (fe * 0.85 + cu * 0.15 + cr * 0.15)
    G = D * (fe * 0.30 + cu * 0.25 + cr * 0.80)
    B = D * (fe * 0.10 + cu * 0.85 + cr * 0.20)

    img = np.stack([R, G, B], axis=-1)
    img = np.clip(img * 1.5, 0, 1)  # boost brightness

    fig, ax = plt.subplots(1, 1, figsize=(10, 10), facecolor='#080808')
    ax.set_facecolor('#080808')
    ax.imshow(img, extent=[-extent, extent, -extent, extent],
              origin='lower', interpolation='bilinear')

    ax.set_xlim(-extent, extent)
    ax.set_ylim(-extent, extent)
    ax.set_aspect('equal')
    ax.set_title(title, color='#c0d8f0', fontsize=14, pad=10)
    ax.tick_params(colors='#405060', labelsize=7)
    for spine in ax.spines.values():
        spine.set_color('#1a2a3a')

    # Legend
    ax.text(extent * 0.65, -extent * 0.92, '■ Ferrite', color='#d9804020', fontsize=9)
    ax.text(extent * 0.65, -extent * 0.96, '■ Cuprite', color='#4080d9', fontsize=9)
    ax.text(extent * 0.65, -extent * 1.00, '■ Crystal', color='#40cc50', fontsize=9)

    plt.tight_layout()
    plt.savefig(filename, dpi=150, facecolor='#080808', bbox_inches='tight')
    plt.close()
    print(f"  saved {filename}")


if __name__ == '__main__':
    outdir = 'docs/belt_samples'
    os.makedirs(outdir, exist_ok=True)

    seeds = [432, 1337, 2037, 8080]
    for seed in seeds:
        print(f"Seed {seed}:")
        render_density_map(seed, f'{outdir}/density_{seed}.png',
                          f'Asteroid Density — Seed {seed}')
        render_ore_map(seed, f'{outdir}/ore_{seed}.png',
                      f'Ore Distribution — Seed {seed}')

    print(f"\nDone. {len(seeds)} seeds × 2 maps = {len(seeds)*2} images in {outdir}/")
