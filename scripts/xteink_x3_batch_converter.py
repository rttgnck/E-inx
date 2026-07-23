from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, Literal

from PIL import Image, ImageEnhance, ImageOps

TARGET_SIZE = (528, 792)
GRAYSCALE_LEVELS = (0, 85, 170, 255)

SUPPORTED_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".bmp", ".webp",
    ".tif", ".tiff", ".gif"
}

FitMode = Literal["crop", "contain", "stretch"]


def choose_directory(title: str) -> Path | None:
    """Open a native folder-selection dialog."""
    try:
        import tkinter as tk
        from tkinter import filedialog
    except ImportError:
        print("Tkinter is unavailable. Pass folders on the command line instead.")
        return None

    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    selected = filedialog.askdirectory(title=title)
    root.destroy()

    return Path(selected) if selected else None


def prepare_size(
    image: Image.Image,
    fit_mode: FitMode,
    background: int = 255,
) -> Image.Image:
    """Resize an image to exactly 528x792."""
    image = ImageOps.exif_transpose(image)

    if fit_mode == "crop":
        # Fill the entire screen and crop equally from opposing edges.
        return ImageOps.fit(
            image,
            TARGET_SIZE,
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )

    if fit_mode == "contain":
        # Preserve the complete image and add borders where needed.
        contained = ImageOps.contain(
            image,
            TARGET_SIZE,
            method=Image.Resampling.LANCZOS,
        )

        canvas = Image.new("RGB", TARGET_SIZE, (background,) * 3)
        x = (TARGET_SIZE[0] - contained.width) // 2
        y = (TARGET_SIZE[1] - contained.height) // 2

        if contained.mode == "RGBA":
            canvas.paste(contained, (x, y), contained)
        else:
            canvas.paste(contained.convert("RGB"), (x, y))

        return canvas

    if fit_mode == "stretch":
        # Force the image to the target dimensions.
        return image.resize(TARGET_SIZE, Image.Resampling.LANCZOS)

    raise ValueError(f"Unknown fit mode: {fit_mode}")


def quantize_four_grays(image: Image.Image) -> Image.Image:
    """
    Convert to four fixed grayscale levels with Floyd-Steinberg dithering.

    Pillow first performs a one-bit Floyd-Steinberg conversion for spatial
    dithering. The result is blended with the source luminance to retain four
    useful e-paper tones, then snapped to 0, 85, 170, or 255.
    """
    gray = image.convert("L")

    # Mild contrast boost helps images remain readable on e-paper.
    gray = ImageEnhance.Contrast(gray).enhance(1.12)

    # Pillow's built-in adaptive four-color palette with FS dithering.
    dithered = gray.quantize(
        colors=4,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.FLOYDSTEINBERG,
    ).convert("L")

    # Snap adaptive palette values to four predictable X3 grayscale values.
    lookup = []
    for value in range(256):
        nearest = min(GRAYSCALE_LEVELS, key=lambda level: abs(level - value))
        lookup.append(nearest)

    return dithered.point(lookup, mode="L")


def four_gray_image_to_2bit_indexes(image: Image.Image) -> Image.Image:
    """Return a palette-index image where pixels are exactly 0..3."""
    gray = image.convert("L")
    return gray.point(
        lambda value: min(range(4), key=lambda level: abs(GRAYSCALE_LEVELS[level] - value)),
        mode="P",
    )


def indexed_image_to_2bit_bmp(image: Image.Image) -> bytes:
    """
    Write an uncompressed top-down 2-bit BMP for the X3 firmware.

    The firmware's BMP reader supports this format directly:
    528x792, 2 bits per pixel, four grayscale palette entries, no compression.
    """
    indexed = four_gray_image_to_2bit_indexes(image)
    width, height = indexed.size
    row_bytes = ((width * 2 + 31) // 32) * 4
    pixel_bytes = row_bytes * height
    palette_bytes = bytes(
        value
        for level in GRAYSCALE_LEVELS
        for value in (level, level, level, 0)
    )
    pixel_offset = 14 + 40 + len(palette_bytes)
    file_size = pixel_offset + pixel_bytes

    header = bytearray()
    header.extend(b"BM")
    header.extend(file_size.to_bytes(4, "little"))
    header.extend((0).to_bytes(4, "little"))
    header.extend(pixel_offset.to_bytes(4, "little"))

    dib = bytearray()
    dib.extend((40).to_bytes(4, "little"))
    dib.extend(width.to_bytes(4, "little", signed=True))
    dib.extend((-height).to_bytes(4, "little", signed=True))
    dib.extend((1).to_bytes(2, "little"))
    dib.extend((2).to_bytes(2, "little"))
    dib.extend((0).to_bytes(4, "little"))
    dib.extend(pixel_bytes.to_bytes(4, "little"))
    dib.extend((2835).to_bytes(4, "little", signed=True))
    dib.extend((2835).to_bytes(4, "little", signed=True))
    dib.extend((4).to_bytes(4, "little"))
    dib.extend((4).to_bytes(4, "little"))

    pixels = indexed.load()
    rows = bytearray()
    for y in range(height):
        row = bytearray(row_bytes)
        out_index = 0
        shift = 6
        current = 0
        for x in range(width):
            current |= (int(pixels[x, y]) & 0x03) << shift
            if shift == 0:
                row[out_index] = current
                out_index += 1
                shift = 6
                current = 0
            else:
                shift -= 2
        if shift != 6:
            row[out_index] = current
        rows.extend(row)

    return bytes(header + dib) + palette_bytes + bytes(rows)


def convert_one(
    source: Path,
    output_dir: Path,
    fit_mode: FitMode,
    output_format: str,
    background: int,
    invert: bool,
) -> list[Path]:
    """Convert one image and return the generated paths."""
    with Image.open(source) as opened:
        # Use the first frame of animated images.
        opened.seek(0)
        image = opened.convert("RGBA")

        # Flatten transparency before resizing.
        flattened = Image.new("RGBA", image.size, (background,) * 3 + (255,))
        flattened.alpha_composite(image)
        prepared = prepare_size(flattened.convert("RGB"), fit_mode, background)

    converted = quantize_four_grays(prepared)

    if invert:
        converted = ImageOps.invert(converted)

    output_dir.mkdir(parents=True, exist_ok=True)
    stem = source.stem
    generated: list[Path] = []

    formats = ("png", "bmp") if output_format == "both" else (output_format,)

    for fmt in formats:
        destination = output_dir / f"{stem}_x3_528x792.{fmt}"

        if fmt == "png":
            converted.save(destination, format="PNG", optimize=True)
        elif fmt == "bmp":
            destination.write_bytes(indexed_image_to_2bit_bmp(converted))
        else:
            raise ValueError(f"Unsupported output format: {fmt}")

        generated.append(destination)

    return generated


def find_images(folder: Path, recursive: bool) -> Iterable[Path]:
    iterator = folder.rglob("*") if recursive else folder.iterdir()

    for path in sorted(iterator):
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS:
            yield path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch-convert images for the 528x792 XTEINK X3 e-reader."
    )
    parser.add_argument(
        "input_folder",
        nargs="?",
        type=Path,
        help="Folder containing source images. A folder picker opens if omitted.",
    )
    parser.add_argument(
        "output_folder",
        nargs="?",
        type=Path,
        help="Destination folder. Defaults to an XTEINK_X3 folder inside input.",
    )
    parser.add_argument(
        "--fit",
        choices=("crop", "contain", "stretch"),
        default="crop",
        help="Resize behavior. Default: crop.",
    )
    parser.add_argument(
        "--format",
        choices=("bmp", "png", "both"),
        default="bmp",
        help="Output format. Default: bmp (native 2-bit X3 sleep image).",
    )
    parser.add_argument(
        "--background",
        choices=("white", "black"),
        default="white",
        help="Border/transparency background for contain mode. Default: white.",
    )
    parser.add_argument(
        "--invert",
        action="store_true",
        help="Explicitly invert the final grayscale image. By default, uploaded light/dark tone is preserved.",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Also process images in subfolders.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    input_dir = args.input_folder or choose_directory(
        "Select the folder containing images"
    )
    if input_dir is None:
        print("No input folder selected.")
        return 1

    input_dir = input_dir.expanduser().resolve()
    if not input_dir.is_dir():
        print(f"Input folder does not exist: {input_dir}")
        return 1

    if args.output_folder:
        output_dir = args.output_folder.expanduser().resolve()
    elif args.input_folder is None:
        chosen_output = choose_directory(
            "Select the output folder, or cancel to use XTEINK_X3 inside the input folder"
        )
        output_dir = chosen_output or (input_dir / "XTEINK_X3")
    else:
        output_dir = input_dir / "XTEINK_X3"

    background = 255 if args.background == "white" else 0
    images = list(find_images(input_dir, args.recursive))

    # Avoid processing files already placed in the default output directory.
    images = [
        path for path in images
        if output_dir not in path.parents
    ]

    if not images:
        print(f"No supported images found in: {input_dir}")
        return 1

    print(f"Input:  {input_dir}")
    print(f"Output: {output_dir}")
    print(f"Images: {len(images)}")
    print()

    converted_count = 0
    failed_count = 0

    for index, source in enumerate(images, start=1):
        try:
            generated = convert_one(
                source=source,
                output_dir=output_dir,
                fit_mode=args.fit,
                output_format=args.format,
                background=background,
                invert=args.invert,
            )
            converted_count += 1
            names = ", ".join(path.name for path in generated)
            print(f"[{index}/{len(images)}] OK   {source.name} -> {names}")
        except Exception as exc:
            failed_count += 1
            print(f"[{index}/{len(images)}] FAIL {source.name}: {exc}")

    print()
    print(f"Finished: {converted_count} converted, {failed_count} failed.")
    print(f"Saved to: {output_dir}")

    return 0 if failed_count == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
