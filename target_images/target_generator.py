#!/usr/bin/env python3

from PIL import Image, ImageDraw, ImageFont
from pathlib import Path
import argparse


# ============================================================
# Configuration
# ============================================================

DPI = 96

IMAGE_WIDTH = 4 * DPI       # 384 px
IMAGE_HEIGHT = 6 * DPI      # 576 px

MARGIN_TOP = int(0.75 * DPI)       # 72 px
MARGIN_BOTTOM = int(0.75 * DPI)    # 72 px

# The entire text/indicator column is on the left.
LEFT_COLUMN_WIDTH = 45

# Pharmacode dimensions
#
# Requested ratio:
#
#   narrow bar = 6
#   space      = 13
#   wide bar   = 18
#
NARROW_BAR_WIDTH = 6
BAR_GAP = 13
WIDE_BAR_WIDTH = 18

# Height of the horizontal barcode
BARCODE_HEIGHT = IMAGE_WIDTH - LEFT_COLUMN_WIDTH -10

BACKGROUND = "white"
BAR_COLOR = "black"


# ============================================================
# Pharmacode encoding
# ============================================================

def pharmacode_bars(value):
    """
    Generate the Pharmacode bar sequence from the LSB end.

        even -> wide
        odd  -> narrow

    The returned list is ordered from LSB end to MSB end.
    """

    if value < 79 or value > 94:
        raise ValueError(
            "This value will not return a valid target for the game. values 79-94 map to targets 0-15"
        )

    bars = []

    while value != 0:

        if value % 2 == 0:
            bars.append("wide")
            value = (value - 2) // 2

        else:
            bars.append("narrow")
            value = (value - 1) // 2

    return bars


# ============================================================
# Font helper
# ============================================================

def get_font(size, bold=False):

    if bold:
        candidates = [
            r"C:\Windows\Fonts\arialbd.ttf",
            r"C:\Windows\Fonts\segoeuib.ttf",
        ]
    else:
        candidates = [
            r"C:\Windows\Fonts\arial.ttf",
            r"C:\Windows\Fonts\segoeui.ttf",
        ]

    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size)

    return ImageFont.load_default()


# ============================================================
# Draw Pharmacode
# ============================================================

def draw_pharmacode(value, output_path):

    image = Image.new(
        "RGB",
        (IMAGE_WIDTH, IMAGE_HEIGHT),
        BACKGROUND
    )

    draw = ImageDraw.Draw(image)

    # --------------------------------------------------------
    # Generate bars
    # --------------------------------------------------------

    bars = pharmacode_bars(value)

    # --------------------------------------------------------
    # Barcode position
    # --------------------------------------------------------

    barcode_x = LEFT_COLUMN_WIDTH

    usable_height = (
        IMAGE_HEIGHT
        - MARGIN_TOP
        - MARGIN_BOTTOM
    )

    # --------------------------------------------------------
    # Draw barcode in its original orientation
    # --------------------------------------------------------
    #
    # The barcode is first drawn horizontally exactly as in the
    # original working version.
    #
    # It is then rotated 90 degrees counterclockwise.
    #

    total_bar_width = sum(
        NARROW_BAR_WIDTH
        if bar == "narrow"
        else WIDE_BAR_WIDTH
        for bar in bars
    )

    total_gap_width = (
        BAR_GAP * (len(bars) - 1)
    )

    barcode_width = (
        total_bar_width
        + total_gap_width
    )

    # need to compare barcode_width to usable_height and scale it because different codes result in a different barcode_width

    bar_width_scale_factor = (usable_height / barcode_width)

    print(f"bar_width_scale_factor: {bar_width_scale_factor}")

    #now recalculate the widths for the total image size

    total_bar_width = sum(
        (NARROW_BAR_WIDTH * bar_width_scale_factor)
        if bar == "narrow"
        else (WIDE_BAR_WIDTH * bar_width_scale_factor)
        for bar in bars
    )

    total_gap_width = (
        (BAR_GAP * bar_width_scale_factor) * (len(bars) - 1)
    )

    barcode_width = (
        total_bar_width
        + total_gap_width
    )

    barcode_image = Image.new(
        "RGB",
        (
            int(barcode_width),
            BARCODE_HEIGHT
        ),
        BACKGROUND
    )

    barcode_draw = ImageDraw.Draw(
        barcode_image
    )

    x = 0

    for bar in bars:

        if bar == "narrow":
            width = (NARROW_BAR_WIDTH * bar_width_scale_factor)
        else:
            width = (WIDE_BAR_WIDTH * bar_width_scale_factor)

        barcode_draw.rectangle(
            [
                x,
                0,
                x + width - 1,
                BARCODE_HEIGHT - 1
            ],
            fill=BAR_COLOR
        )

        x += width + (BAR_GAP * bar_width_scale_factor)

    # --------------------------------------------------------
    # Rotate barcode 90 degrees counterclockwise
    # --------------------------------------------------------

    barcode_image = barcode_image.rotate(
        90,
        expand=True
    )

    # --------------------------------------------------------
    # Paste rotated barcode
    # --------------------------------------------------------

    image.paste(
        barcode_image,
        (
            barcode_x,
            MARGIN_TOP
        )
    )

    # --------------------------------------------------------
    # Left-side vertical text column
    # --------------------------------------------------------

    label_font = get_font(16, bold=True)
    decimal_font = get_font(22, bold=True)

    column_center_x = LEFT_COLUMN_WIDTH // 2

    # --------------------------------------------------------
    # "THIS END UP"
    # --------------------------------------------------------

    label = "THIS END UP"

    label_bbox = draw.textbbox(
        (0, 0),
        label,
        font=label_font
    )

    label_width = (
        label_bbox[2] - label_bbox[0]
    )

    label_height = (
        label_bbox[3] - label_bbox[1]
    )

    label_layer = Image.new(
        "RGBA",
        (
            label_width + 10,
            label_height + 10
        ),
        (255, 255, 255, 0)
    )

    label_draw = ImageDraw.Draw(
        label_layer
    )

    label_draw.text(
        (5, 5),
        label,
        fill=BAR_COLOR,
        font=label_font
    )

    label_layer = label_layer.rotate(
        90,
        expand=True
    )

    label_x = (
        column_center_x
        - label_layer.width // 2
    )

    label_y = (
        MARGIN_TOP
    )

    image.paste(
        label_layer,
        (label_x, label_y),
        label_layer
    )

    # --------------------------------------------------------
    # Decimal value
    # --------------------------------------------------------

    decimal_text = str(value - 79)

    decimal_bbox = draw.textbbox(
        (0, 0),
        decimal_text,
        font=decimal_font
    )

    decimal_width = (
        decimal_bbox[2] - decimal_bbox[0]
    )

    decimal_x = (
        column_center_x
        - decimal_width // 2
    )

    decimal_y = (
        label_y
        + label_layer.height
        + 100
    )

    draw.text(
        (
            decimal_x,
            decimal_y
        ),
        decimal_text,
        fill=BAR_COLOR,
        font=decimal_font
    )

    # --------------------------------------------------------
    # UP arrow
    # --------------------------------------------------------

    arrow_x = column_center_x

    arrow_bottom = MARGIN_TOP
    arrow_top = MARGIN_TOP -30

    draw.line(
        [
            arrow_x,
            arrow_bottom,
            arrow_x,
            arrow_top
        ],
        fill=BAR_COLOR,
        width=3
    )

    draw.polygon(
        [
            (arrow_x, arrow_top - 8),
            (arrow_x - 8, arrow_top + 8),
            (arrow_x + 8, arrow_top + 8)
        ],
        fill=BAR_COLOR
    )

    # --------------------------------------------------------
    # Save
    # --------------------------------------------------------

    image.save(
        output_path,
        dpi=(DPI, DPI)
    )

    print(f"Created: {output_path}")


# ============================================================
# Main
# ============================================================

def main():

    parser = argparse.ArgumentParser(
        description="Generate a target for LaserTag V3. (horizontal Pharmacode image)"
    )

    parser.add_argument(
        "value",
        type=int,
        help="Pharmacode decimal value"
    )

    parser.add_argument(
        "-o",
        "--output",
        default=None,
        help="Output PNG filename"
    )

    args = parser.parse_args()

    if args.output is None:
        args.output = (
            f"pharmacode_{args.value}.png"
        )

    draw_pharmacode(
        args.value,
        args.output
    )


if __name__ == "__main__":
    main()