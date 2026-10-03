#!/usr/bin/env python3
# Copyright 2016 Emilie Gillet.
#
# Author: Emilie Gillet (emilie.o.gillet@gmail.com)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
# See http://creativecommons.org/licenses/MIT/ for more information.
#
# Adapted for the Disting NT by Andras Eichstaedt, 2026.
# Changes: extracts the 15 integrated wavetables used by Plaits' chord engine
# out of plaits/resources.cc and emits them as a standalone C++ header
# (plaits_chord_wavetables.h) for the Disting NT chord plugin fork. No sample
# data is modified -- values are copied verbatim from the upstream table.
#
# -----------------------------------------------------------------------------
#
# Usage:
#   python3 generate_chord_wavetables.py --resources <path to plaits/resources.cc> \
#       --out plaits_chord_wavetables.h
#
# The 15 (bank, row, column) tuples below are exactly the WAVE(...) entries
# in plaits/dsp/engine/chord_engine.cc's `wavetable[]` table, in the same
# order (this is the MORPH scan order -- do not reorder).

import argparse
import re
import sys

WAVES = [
    (2, 6, 1),
    (2, 6, 6),
    (2, 6, 4),
    (0, 6, 0),
    (0, 6, 1),
    (0, 6, 2),
    (0, 6, 7),
    (2, 4, 7),
    (2, 4, 6),
    (2, 4, 5),
    (2, 4, 4),
    (2, 4, 3),
    (2, 4, 2),
    (2, 4, 1),
    (2, 4, 0),
]

WAVE_SIZE = 132  # 128 samples + 4 guard samples, per upstream layout
BANK_STRIDE = 64  # 8 rows * 8 columns per bank

ATTRIBUTION = '''// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// Adapted for the Disting NT by Andras Eichstaedt, 2026.
// Changes: 15 of Plaits' integrated wavetables extracted verbatim from
// resources.cc via generate_chord_wavetables.py; no sample data modified.
//
// -----------------------------------------------------------------------------
//
// GENERATED FILE -- do not hand-edit. Regenerate with generate_chord_wavetables.py.
'''


def extract_flat_table(resources_path):
    with open(resources_path, 'r') as f:
        text = f.read()
    m = re.search(
        r'const int16_t wav_integrated_waves\[\]\s*=\s*\{(.*?)\n\};',
        text, re.DOTALL)
    if not m:
        sys.exit("Could not find 'wav_integrated_waves' array in %s" % resources_path)
    body = m.group(1)
    flat = [int(x) for x in re.findall(r'-?\d+', body)]
    expected = 3 * BANK_STRIDE * WAVE_SIZE  # 3 banks * 64 slots/bank * 132 samples
    if len(flat) != expected:
        sys.exit("Expected %d entries in wav_integrated_waves, found %d" %
                  (expected, len(flat)))
    return flat


def slice_wave(flat, bank, row, column):
    base = (bank * BANK_STRIDE + row * 8 + column) * WAVE_SIZE
    if base + WAVE_SIZE > len(flat):
        sys.exit("Wave (%d,%d,%d) out of bounds" % (bank, row, column))
    return flat[base:base + WAVE_SIZE]


def format_table(values, indent='    '):
    lines = []
    for i in range(0, len(values), 12):
        chunk = values[i:i + 12]
        lines.append(indent + ', '.join('%6d' % v for v in chunk) + ',')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--resources', required=True,
                     help='path to plaits/resources.cc')
    ap.add_argument('--out', default='plaits_chord_wavetables.h',
                     help='output header path')
    args = ap.parse_args()

    flat = extract_flat_table(args.resources)

    tables = [slice_wave(flat, bank, row, column) for (bank, row, column) in WAVES]

    with open(args.out, 'w') as f:
        f.write(ATTRIBUTION)
        f.write('\n#ifndef PLAITS_CHORD_WAVETABLES_H_\n')
        f.write('#define PLAITS_CHORD_WAVETABLES_H_\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('static const int16_t chord_wavetables[15][132] = {\n')
        for idx, table in enumerate(tables):
            bank, row, column = WAVES[idx]
            f.write('  // wave %d: (bank=%d, row=%d, column=%d)\n' % (idx, bank, row, column))
            f.write('  {\n')
            f.write(format_table(table, indent='    '))
            f.write('\n  },\n')
        f.write('};\n\n')
        f.write('static const int16_t* const chord_wavetable_ptrs[15] = {\n')
        for i in range(15):
            f.write('  chord_wavetables[%d],\n' % i)
        f.write('};\n\n')
        f.write('#endif  // PLAITS_CHORD_WAVETABLES_H_\n')

    print("Wrote %d tables (%d int16 values each) to %s" %
          (len(tables), WAVE_SIZE, args.out))


if __name__ == '__main__':
    main()
