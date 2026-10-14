"""Reproduce the published local temporal metric from original MRQ PNGs.

Dependencies: numpy, Pillow, scipy. The default regions/frame range describe
the Garden Villa validation; edit REGIONS for another scene or framing.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, shift
from scipy.optimize import minimize


REGIONS = {
    '1_near_pillar': ((275, 675, 365, 810), (321, 725, 336, 780)),
    '2_gate_left': ((630, 665, 710, 780), (670, 690, 685, 750)),
    '3_gate_right': ((890, 665, 945, 780), (910, 694, 935, 742)),
    '4_building_junction': ((1000, 630, 1090, 780), (1033, 650, 1050, 735)),
    '5_facade_left': ((1065, 560, 1190, 775), (1088, 680, 1140, 740)),
    '6_facade_right': ((1500, 560, 1620, 775), (1540, 680, 1595, 740)),
}


def load_crop(directory, pattern, number, rect):
    with Image.open(directory / pattern.format(frame=number)) as image:
        if image.size != (2560, 1440):
            raise ValueError('Default regions require 2560 x 1440 images.')
        pixels = np.asarray(image.convert('RGB').crop(rect), dtype=np.float32)
    return pixels @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def measure(directory, pattern, frames, reg, patch):
    rows = []
    previous = load_crop(directory, pattern, frames[0], reg)
    x0, y0, x1, y1 = patch
    selected = (slice(y0 - reg[1], y1 - reg[1]), slice(x0 - reg[0], x1 - reg[0]))
    for number in frames[1:]:
        current = load_crop(directory, pattern, number, reg)
        a, b = gaussian_filter(previous, 2), gaussian_filter(current, 2)

        def objective(offset):
            difference = (shift(b, offset, order=1, mode='nearest') - a)[10:-10, 10:-10]
            return np.mean((difference - difference.mean()) ** 2)

        fit = minimize(objective, [0, 0], method='Powell',
                       bounds=[(-8, 8), (-8, 8)], options={'xtol': 0.01})
        aligned = shift(current, fit.x, order=1, mode='nearest')
        difference = (aligned - previous)[selected]
        row = {'frame': number, 'shift_yx': fit.x.tolist(),
               'mean_delta': float(difference.mean()),
               'absolute_mean_delta': float(abs(difference.mean())),
               'registered_mae_8bit': float(np.abs(difference).mean()),
               'registered_rms_8bit': float(np.sqrt(np.mean(difference * difference)))}
        for sigma in [1, 4, 8]:
            filtered = (gaussian_filter(aligned, sigma) - gaussian_filter(previous, sigma))[selected]
            row[f'registered_sigma{sigma}_rms_8bit'] = float(np.sqrt(np.mean(filtered * filtered)))
        rows.append(row)
        previous = current
    means = {key: float(np.mean([row[key] for row in rows]))
             for key in rows[0] if key not in ['frame', 'shift_yx']}
    return {'pairs': rows, 'mean': means}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pattern', default='.{frame:04d}.png',
                        help='Filename pattern, e.g. Shot.{frame:04d}.png')
    parser.add_argument('--start', type=int, default=55)
    parser.add_argument('--end', type=int, default=65, help='Inclusive last frame')
    args = parser.parse_args()
    if args.end <= args.start:
        parser.error('--end must be greater than --start')
    frames = list(range(args.start, args.end + 1))
    report = {'frames': frames, 'regions': {}}
    for name, (reg, patch) in REGIONS.items():
        before = measure(args.before, args.pattern, frames, reg, patch)
        after = measure(args.after, args.pattern, frames, reg, patch)
        keys = ['absolute_mean_delta', 'registered_sigma4_rms_8bit', 'registered_sigma8_rms_8bit']
        reductions = {key: 100 * (1 - after['mean'][key] / before['mean'][key])
                      if before['mean'][key] else None for key in keys}
        report['regions'][name] = {'registration_rect': reg, 'measurement_rect': patch,
                                   'before': before, 'after': after, 'reduction_percent': reductions}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
