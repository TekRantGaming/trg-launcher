"""Turns a PNG into a C++ header for TRG Launcher 2.0's logo (ProjectConfig::logo_png).

    python png_to_header.py logo.png logo_png.h kMyLogoPng

Then in project.cpp:  p.logo_png = kMyLogoPng;  p.logo_png_size = sizeof kMyLogoPng;
"""
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    src, out, name = sys.argv[1:]
    data = open(src, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        print(src + ' is not a PNG')
        return 1
    lines = ['// ' + src.replace('\\', '/').split('/')[-1] + ', as a PNG (made with launcher2/tools/png_to_header.py). Generated.',
             '#pragma once', '', 'inline constexpr unsigned char %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), 24):
        lines.append('    ' + ','.join('0x%02X' % b for b in data[i:i + 24]) + ',')
    lines.append('};')
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%s: %d bytes' % (out, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
