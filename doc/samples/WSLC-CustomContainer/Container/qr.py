#!/usr/bin/env python3
"""Turn text (e.g. a URL) into a scannable QR code printed to the terminal.

Used by the WSLC-CustomContainer sample. The Windows host passes the text to
encode as command-line arguments; the QR is drawn with Unicode block characters
so it scans straight from the terminal.
"""

import sys

import qrcode


def main() -> int:
    text = " ".join(sys.argv[1:]).strip()
    if not text:
        print("usage: qr.py <text-or-url>", file=sys.stderr)
        return 2

    qr = qrcode.QRCode(border=2)
    qr.add_data(text)
    qr.make(fit=True)

    print(f"QR code for: {text}\n")
    # invert=True renders dark modules as spaces on a light background, which
    # scans reliably in terminals with a dark color scheme.
    qr.print_ascii(invert=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
