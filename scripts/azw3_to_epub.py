#!/usr/bin/env python3
"""Pick an AZW3/MOBI file and convert it to EPUB with Calibre."""

from __future__ import annotations

import argparse
import html
import io
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime
from pathlib import Path
from urllib.parse import unquote
from xml.etree import ElementTree
from dataclasses import dataclass


SUPPORTED_EXTENSIONS = {".azw3", ".mobi", ".azw", ".kf8"}
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
DEFAULT_CROP_MARGIN = 8
DEFAULT_BLACK_THRESHOLD = 28
DEFAULT_EDGE_COVERAGE = 0.985
DEFAULT_JPEG_QUALITY = 82
DEFAULT_BW_WIDTH = 528
DEFAULT_BW_HEIGHT = 792
DEFAULT_BW_PAGES_PER_SECTION = 16


@dataclass(frozen=True)
class GuidedCrop:
    viewport_w: float
    viewport_h: float
    image_left: float
    image_top: float
    image_w: float
    image_h: float


def find_ebook_convert() -> str | None:
    env_path = os.environ.get("EBOOK_CONVERT")
    if env_path and Path(env_path).is_file():
        return env_path

    found = shutil.which("ebook-convert")
    if found:
        return found

    for candidate in (
        "/Applications/calibre.app/Contents/MacOS/ebook-convert",
        "/Applications/Calibre.app/Contents/MacOS/ebook-convert",
        "/opt/homebrew/bin/ebook-convert",
        "/usr/local/bin/ebook-convert",
    ):
        if Path(candidate).is_file():
            return candidate

    return None


def find_calibre_debug() -> str | None:
    found = shutil.which("calibre-debug")
    if found:
        return found

    for candidate in (
        "/Applications/calibre.app/Contents/MacOS/calibre-debug",
        "/Applications/Calibre.app/Contents/MacOS/calibre-debug",
        "/opt/homebrew/bin/calibre-debug",
        "/usr/local/bin/calibre-debug",
    ):
        if Path(candidate).is_file():
            return candidate

    return None


def calibre_env() -> dict[str, str]:
    calibre_scratch = Path(tempfile.gettempdir()) / "inx-azw3-to-epub-calibre"
    config_dir = calibre_scratch / "config"
    cache_dir = calibre_scratch / "cache"
    temp_dir = calibre_scratch / "tmp"
    for directory in (config_dir, cache_dir, temp_dir):
        directory.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.setdefault("CALIBRE_CONFIG_DIRECTORY", str(config_dir))
    env.setdefault("CALIBRE_CACHE_DIRECTORY", str(cache_dir))
    env.setdefault("CALIBRE_TEMP_DIR", str(temp_dir))
    return env


def pick_with_osascript() -> Path | None:
    script = (
        'POSIX path of (choose file with prompt '
        '"Select an AZW3/MOBI book to convert to EPUB")'
    )
    try:
        result = subprocess.run(
            ["osascript", "-e", script],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return None

    if result.returncode != 0:
        return None

    selected = result.stdout.strip()
    return Path(selected) if selected else None


def pick_with_tkinter() -> Path | None:
    try:
        import tkinter as tk
        from tkinter import filedialog
    except Exception:
        return None

    root = tk.Tk()
    root.withdraw()
    try:
        selected = filedialog.askopenfilename(
            title="Select an AZW3/MOBI book to convert to EPUB",
            filetypes=[
                ("Kindle books", "*.azw3 *.mobi *.azw *.kf8"),
                ("All files", "*"),
            ],
        )
    finally:
        root.destroy()

    return Path(selected) if selected else None


def pick_input_file() -> Path:
    selected = pick_with_osascript() if sys.platform == "darwin" else None
    if selected is None:
        selected = pick_with_tkinter()
    if selected is None:
        raise SystemExit("No file selected.")
    return selected


def choose_output_path(input_path: Path, requested: str | None, overwrite: bool) -> Path:
    if requested:
        output_path = Path(requested).expanduser()
        if output_path.is_dir():
            output_path = output_path / f"{input_path.stem}.epub"
    else:
        output_path = input_path.with_suffix(".epub")

    if output_path.suffix.lower() != ".epub":
        output_path = output_path.with_suffix(".epub")

    if overwrite or not output_path.exists():
        return output_path

    base = output_path.with_suffix("")
    for idx in range(2, 1000):
        candidate = base.with_name(f"{base.name}-converted-{idx}").with_suffix(".epub")
        if not candidate.exists():
            return candidate

    raise SystemExit(f"Too many converted EPUBs already exist near {output_path}")


def add_fresh_name_suffix(output_path: Path) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return output_path.with_name(f"{output_path.stem}-x3-{stamp}{output_path.suffix}")


def validate_input(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"Input file not found: {path}")
    if path.suffix.lower() not in SUPPORTED_EXTENSIONS:
        allowed = ", ".join(sorted(SUPPORTED_EXTENSIONS))
        raise SystemExit(f"Expected one of: {allowed}")


def convert(input_path: Path, output_path: Path, ebook_convert: str) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    command = [ebook_convert, str(input_path), str(output_path)]
    result = subprocess.run(command, check=False, env=calibre_env())
    if result.returncode != 0:
        if output_path.exists() and output_path.stat().st_size == 0:
            output_path.unlink()
        raise SystemExit(
            "Conversion failed. If this is a Kindle-store book, remove DRM first "
            "using your own legally authorized workflow, then retry."
        )


def import_pillow_image():
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("Pillow is not available in this Python runtime") from exc
    return Image


def image_has_alpha(image) -> bool:
    return image.mode in ("RGBA", "LA") or (
        image.mode == "P" and "transparency" in image.info
    )


def blackish_mask(image, threshold: int):
    rgb = image.convert("RGB")
    px = rgb.load()
    width, height = rgb.size
    mask = [[False] * width for _ in range(height)]

    for y in range(height):
        row = mask[y]
        for x in range(width):
            r, g, b = px[x, y]
            row[x] = r <= threshold and g <= threshold and b <= threshold
    return mask


def black_row(mask: list[list[bool]], y: int, coverage: float) -> bool:
    width = len(mask[y])
    return sum(1 for value in mask[y] if value) >= width * coverage


def black_col(mask: list[list[bool]], x: int, top: int, bottom: int, coverage: float) -> bool:
    height = bottom - top + 1
    return sum(1 for y in range(top, bottom + 1) if mask[y][x]) >= height * coverage


def find_black_border_crop(image, margin: int, threshold: int, coverage: float):
    width, height = image.size
    if width < 8 or height < 8 or image_has_alpha(image):
        return None

    mask = blackish_mask(image, threshold)
    top = 0
    bottom = height - 1
    left = 0
    right = width - 1

    while top <= bottom and black_row(mask, top, coverage):
        top += 1
    while bottom >= top and black_row(mask, bottom, coverage):
        bottom -= 1
    if top > bottom:
        return None

    while left <= right and black_col(mask, left, top, bottom, coverage):
        left += 1
    while right >= left and black_col(mask, right, top, bottom, coverage):
        right -= 1
    if left > right:
        return None

    left = max(0, left - margin)
    top = max(0, top - margin)
    right = min(width - 1, right + margin)
    bottom = min(height - 1, bottom + margin)

    if left == 0 and top == 0 and right == width - 1 and bottom == height - 1:
        return None
    if right - left + 1 < 8 or bottom - top + 1 < 8:
        return None

    return left, top, right + 1, bottom + 1


def crop_image_bytes(
    name: str,
    data: bytes,
    margin: int,
    threshold: int,
    coverage: float,
    jpeg_quality: int,
):
    Image = import_pillow_image()
    from io import BytesIO

    with Image.open(BytesIO(data)) as image:
        crop_box = find_black_border_crop(image, margin, threshold, coverage)
        if crop_box is None:
            return None

        cropped = image.crop(crop_box)
        out = BytesIO()
        ext = Path(name).suffix.lower()
        save_kwargs = {}

        if ext in {".jpg", ".jpeg"}:
            if cropped.mode not in ("RGB", "L"):
                cropped = cropped.convert("RGB")
            save_format = "JPEG"
            save_kwargs = {"quality": jpeg_quality, "optimize": True, "progressive": False}
        elif ext == ".png":
            save_format = "PNG"
            save_kwargs = {"optimize": True}
        elif ext == ".webp":
            save_format = "WEBP"
            save_kwargs = {"quality": 95, "method": 6}
        else:
            save_format = image.format or "BMP"

        cropped.save(out, save_format, **save_kwargs)
        old_w, old_h = image.size
        new_w, new_h = cropped.size
        return out.getvalue(), (old_w, old_h, new_w, new_h)


def crop_epub_images(
    epub_path: Path,
    margin: int,
    threshold: int,
    coverage: float,
    jpeg_quality: int,
) -> tuple[int, int]:
    epub_path = epub_path.resolve()
    temp_path = epub_path.with_suffix(epub_path.suffix + ".tmp")
    changed = 0
    scanned = 0

    with zipfile.ZipFile(epub_path, "r") as source, zipfile.ZipFile(temp_path, "w") as dest:
        infos = source.infolist()

        mimetype_info = next((info for info in infos if info.filename == "mimetype"), None)
        if mimetype_info is not None:
            dest.writestr("mimetype", source.read(mimetype_info), compress_type=zipfile.ZIP_STORED)

        for info in infos:
            if info.filename == "mimetype":
                continue

            data = source.read(info)
            ext = Path(info.filename).suffix.lower()
            if ext in IMAGE_EXTENSIONS and not info.is_dir():
                scanned += 1
                try:
                    result = crop_image_bytes(info.filename, data, margin, threshold, coverage, jpeg_quality)
                except Exception as exc:
                    print(f"Crop skipped: {info.filename}: {exc}", flush=True)
                    result = None

                if result is not None:
                    data, dims = result
                    old_w, old_h, new_w, new_h = dims
                    changed += 1
                    print(
                        f"Cropped {info.filename}: {old_w}x{old_h} -> {new_w}x{new_h}",
                        flush=True,
                    )

            compress_type = info.compress_type
            if compress_type == zipfile.ZIP_STORED and info.filename != "mimetype":
                compress_type = zipfile.ZIP_DEFLATED
            dest.writestr(info, data, compress_type=compress_type)

    temp_path.replace(epub_path)
    return scanned, changed


def crop_epub_images_with_best_runtime(
    epub_path: Path,
    margin: int,
    threshold: int,
    coverage: float,
    jpeg_quality: int,
) -> tuple[int, int] | None:
    try:
        import_pillow_image()
        return crop_epub_images(epub_path, margin, threshold, coverage, jpeg_quality)
    except RuntimeError:
        calibre_debug = find_calibre_debug()
        if not calibre_debug:
            print("Image crop skipped: Pillow/calibre-debug not found.", flush=True)
            return None

        command = [
            calibre_debug,
            "-e",
            str(Path(__file__).resolve()),
            "--",
            "--crop-epub-only",
            str(epub_path),
            "--crop-margin",
            str(margin),
            "--crop-black-threshold",
            str(threshold),
            "--crop-edge-coverage",
            str(coverage),
            "--jpeg-quality",
            str(jpeg_quality),
        ]
        result = subprocess.run(command, check=False, env=calibre_env())
        if result.returncode != 0:
            raise SystemExit("Image crop failed after conversion.")
        return None


def opf_path_from_container(source: zipfile.ZipFile) -> str:
    try:
        container_xml = source.read("META-INF/container.xml")
        root = ElementTree.fromstring(container_xml)
        for elem in root.iter():
            if elem.tag.endswith("rootfile"):
                full_path = elem.attrib.get("full-path")
                if full_path:
                    return full_path
    except Exception:
        pass

    if "content.opf" in source.namelist():
        return "content.opf"
    for name in source.namelist():
        if name.lower().endswith(".opf"):
            return name
    raise SystemExit("Could not find OPF package in EPUB.")


def media_type_for_image(path: str) -> str:
    ext = Path(path).suffix.lower()
    if ext in {".jpg", ".jpeg"}:
        return "image/jpeg"
    if ext == ".png":
        return "image/png"
    if ext == ".webp":
        return "image/webp"
    if ext == ".bmp":
        return "image/bmp"
    return "application/octet-stream"


def resolve_epub_href(base_file: str, href: str) -> str:
    clean = href.split("#", 1)[0].split("?", 1)[0]
    clean = unquote(clean)
    if clean.startswith("/"):
        clean = clean.lstrip("/")
    else:
        clean = posixpath.normpath(posixpath.join(posixpath.dirname(base_file), clean))
    return clean


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_css_declarations(text: str) -> dict[str, str]:
    props: dict[str, str] = {}
    for declaration in text.split(";"):
        if ":" not in declaration:
            continue
        name, value = declaration.split(":", 1)
        props[name.strip().lower()] = value.strip()
    return props


def parse_css_rules(text: str) -> dict[str, dict[str, str]]:
    rules: dict[str, dict[str, str]] = {}
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    for match in re.finditer(r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}", text, flags=re.DOTALL):
        rules.setdefault(match.group(1), {}).update(parse_css_declarations(match.group(2)))
    return rules


def load_css_rules(source: zipfile.ZipFile) -> dict[str, dict[str, str]]:
    rules: dict[str, dict[str, str]] = {}
    for name in source.namelist():
        if not name.lower().endswith(".css"):
            continue
        try:
            css_text = source.read(name).decode("utf-8", errors="replace")
        except Exception:
            continue
        for class_name, props in parse_css_rules(css_text).items():
            rules.setdefault(class_name, {}).update(props)
    return rules


def element_props(elem: ElementTree.Element, css_rules: dict[str, dict[str, str]]) -> dict[str, str]:
    props: dict[str, str] = {}
    for class_name in elem.attrib.get("class", "").split():
        props.update(css_rules.get(class_name, {}))
    props.update(parse_css_declarations(elem.attrib.get("style", "")))
    return props


def css_px(value: str | None) -> float | None:
    if not value:
        return None
    match = re.match(r"^\s*(-?\d+(?:\.\d+)?)px\s*$", value)
    if not match:
        return None
    return float(match.group(1))


def numeric_attr(value: str | None) -> float | None:
    if not value:
        return None
    match = re.match(r"^\s*(-?\d+(?:\.\d+)?)", value)
    if not match:
        return None
    return float(match.group(1))


def target_id_from_magnify(value: str) -> str:
    try:
        parsed = json.loads(value)
        target_id = parsed.get("targetId")
        if isinstance(target_id, str):
            return target_id
    except Exception:
        pass
    match = re.search(r'"targetId"\s*:\s*"([^"]+)"', value)
    return match.group(1) if match else ""


def first_descendant_image(elem: ElementTree.Element) -> ElementTree.Element | None:
    for child in elem.iter():
        if child is not elem and local_name(child.tag) == "img":
            return child
    return None


def find_element_by_id(root: ElementTree.Element, target_id: str) -> ElementTree.Element | None:
    for elem in root.iter():
        if elem.attrib.get("id") == target_id:
            return elem
    return None


def guided_crops_from_html(
    source: zipfile.ZipFile,
    html_path: str,
    css_rules: dict[str, dict[str, str]],
) -> list[tuple[str, GuidedCrop]]:
    try:
        root = ElementTree.fromstring(source.read(html_path))
    except Exception:
        return []

    crops: list[tuple[str, GuidedCrop]] = []
    for anchor in root.iter():
        magnify = anchor.attrib.get("data-app-amzn-magnify")
        if not magnify:
            continue
        target = find_element_by_id(root, target_id_from_magnify(magnify))
        if target is None:
            continue

        for elem in target.iter():
            if local_name(elem.tag) != "div":
                continue
            div_props = element_props(elem, css_rules)
            if div_props.get("overflow", "").lower() != "hidden":
                continue
            image_elem = first_descendant_image(elem)
            if image_elem is None:
                continue

            image_href = image_elem.attrib.get("src")
            if not image_href:
                continue
            image_path = resolve_epub_href(html_path, image_href)
            if image_path not in source.namelist():
                continue

            img_props = element_props(image_elem, css_rules)
            viewport_w = css_px(div_props.get("width"))
            viewport_h = css_px(div_props.get("height"))
            image_w = css_px(img_props.get("width")) or numeric_attr(image_elem.attrib.get("width"))
            image_h = css_px(img_props.get("height")) or numeric_attr(image_elem.attrib.get("height"))
            image_left = css_px(img_props.get("left")) or 0.0
            image_top = css_px(img_props.get("top")) or 0.0
            if not viewport_w or not viewport_h or not image_w or not image_h:
                continue

            crops.append(
                (
                    image_path,
                    GuidedCrop(
                        viewport_w=viewport_w,
                        viewport_h=viewport_h,
                        image_left=image_left,
                        image_top=image_top,
                        image_w=image_w,
                        image_h=image_h,
                    ),
                )
            )
            break
    return crops


def extract_opf_metadata(opf_bytes: bytes) -> str:
    text = opf_bytes.decode("utf-8", errors="replace")
    match = re.search(r"<metadata\b.*?</metadata>", text, flags=re.DOTALL)
    if match:
        metadata = match.group(0)
        metadata = re.sub(
            r'<meta\s+name=["\']cover["\']\s+content=["\'][^"\']+["\']\s*/?>',
            '<meta name="cover" content="cover"/>',
            metadata,
            flags=re.IGNORECASE,
        )
        if 'name="cover"' not in metadata and "name='cover'" not in metadata:
            metadata = metadata.replace("</metadata>", '    <meta name="cover" content="cover"/>\n  </metadata>')
        return metadata

    return """<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Converted Book</dc:title>
    <dc:language>en</dc:language>
    <meta name="cover" content="cover"/>
  </metadata>"""


def ordered_image_pages_from_epub(
    source: zipfile.ZipFile,
    opf_path: str,
    include_guided_crops: bool,
) -> tuple[str, list[tuple[str, GuidedCrop | None]]]:
    opf_bytes = source.read(opf_path)
    root = ElementTree.fromstring(opf_bytes)
    manifest: dict[str, tuple[str, str]] = {}
    spine_ids: list[str] = []
    cover_id = ""
    css_rules = load_css_rules(source) if include_guided_crops else {}

    for elem in root.iter():
        tag = elem.tag.rsplit("}", 1)[-1]
        if tag == "item":
            item_id = elem.attrib.get("id")
            href = elem.attrib.get("href")
            media_type = elem.attrib.get("media-type", "")
            if item_id and href:
                manifest[item_id] = (resolve_epub_href(opf_path, href), media_type)
        elif tag == "itemref":
            idref = elem.attrib.get("idref")
            if idref:
                spine_ids.append(idref)
        elif tag == "meta" and elem.attrib.get("name") == "cover":
            cover_id = elem.attrib.get("content", "")

    names = set(source.namelist())
    ordered: list[tuple[str, GuidedCrop | None]] = []

    for idref in spine_ids:
        entry = manifest.get(idref)
        if not entry:
            continue
        href, media_type = entry
        if media_type.startswith("image/") and href in names:
            ordered.append((href, None))
            continue
        if "html" not in media_type or href not in names:
            continue

        html_text = source.read(href).decode("utf-8", errors="replace")
        seen_in_doc: set[str] = set()
        first_image = ""
        for match in re.finditer(r"<img\b[^>]*\bsrc\s*=\s*['\"]([^'\"]+)['\"]", html_text, flags=re.IGNORECASE):
            image_href = resolve_epub_href(href, match.group(1))
            if image_href in names and image_href not in seen_in_doc:
                ordered.append((image_href, None))
                seen_in_doc.add(image_href)
                if not first_image:
                    first_image = image_href
        if include_guided_crops and first_image:
            for crop_image, crop in guided_crops_from_html(source, href, css_rules):
                if crop_image == first_image:
                    ordered.append((crop_image, crop))

    if not ordered:
        for _item_id, (href, media_type) in manifest.items():
            if media_type.startswith("image/") and href in names:
                ordered.append((href, None))

    cover_entry = manifest.get(cover_id) or manifest.get("cover")
    if cover_entry:
        cover_href, cover_media_type = cover_entry
        if cover_media_type.startswith("image/") and cover_href in names and (cover_href, None) not in ordered:
            ordered.insert(0, (cover_href, None))

    deduped: list[tuple[str, GuidedCrop | None]] = []
    for image_path, crop in ordered:
        if crop is None and deduped and deduped[-1] == (image_path, None):
            continue
        deduped.append((image_path, crop))

    return extract_opf_metadata(opf_bytes), deduped


def quantize_to_ereader_grays(image):
    Image = import_pillow_image()

    palette = Image.new("P", (1, 1))
    values = []
    for level in (0, 85, 170, 255):
        values.extend([level, level, level])
    values.extend([255, 255, 255] * (256 - 4))
    palette.putpalette(values)

    return image.convert("RGB").quantize(
        palette=palette,
        dither=Image.Dither.FLOYDSTEINBERG,
    )


def page_box_for_guided_crop(image_w: int, image_h: int, crop: GuidedCrop) -> tuple[int, int, int, int] | None:
    left = int(round((-crop.image_left / crop.image_w) * image_w))
    top = int(round((-crop.image_top / crop.image_h) * image_h))
    right = int(round(((crop.viewport_w - crop.image_left) / crop.image_w) * image_w))
    bottom = int(round(((crop.viewport_h - crop.image_top) / crop.image_h) * image_h))

    left = max(0, min(image_w - 1, left))
    top = max(0, min(image_h - 1, top))
    right = max(left + 1, min(image_w, right))
    bottom = max(top + 1, min(image_h, bottom))

    crop_area = (right - left) * (bottom - top)
    full_area = image_w * image_h
    if crop_area >= full_area * 0.92:
        return None
    if right - left < 24 or bottom - top < 24:
        return None
    return left, top, right, bottom


def scale_to_fit(image, width: int, height: int):
    Image = import_pillow_image()

    ratio = min(width / image.width, height / image.height)
    new_w = max(1, min(width, int(round(image.width * ratio))))
    new_h = max(1, min(height, int(round(image.height * ratio))))
    if (new_w, new_h) == image.size:
        return image.copy()
    return image.resize((new_w, new_h), Image.Resampling.LANCZOS)


def bw_page_from_image(
    image,
    original_size: tuple[int, int],
    margin: int,
    threshold: int,
    coverage: float,
    width: int,
    height: int,
) -> tuple[bytes, tuple[int, int, int, int]]:
    Image = import_pillow_image()
    from PIL import ImageEnhance, ImageFilter, ImageOps

    # Preserve native pixels through crop and scaling; dither only after final sizing.
    image = image.convert("RGB")
    crop_box = find_black_border_crop(image, margin, threshold, coverage)
    if crop_box is not None:
        image = image.crop(crop_box)

    gray = ImageOps.grayscale(image)
    gray = scale_to_fit(gray, width, height)
    gray = ImageOps.autocontrast(gray, cutoff=1)
    gray = ImageEnhance.Contrast(gray).enhance(1.18)
    gray = gray.filter(ImageFilter.UnsharpMask(radius=1.1, percent=155, threshold=3))

    canvas = Image.new("L", (width, height), 0)
    x = (width - gray.width) // 2
    y = (height - gray.height) // 2
    canvas.paste(gray, (x, y))

    dithered = quantize_to_ereader_grays(canvas)
    return indexed_image_to_2bit_bmp(dithered), (
        original_size[0],
        original_size[1],
        gray.width,
        gray.height,
    )


def indexed_image_to_2bit_bmp(image) -> bytes:
    width, height = image.size
    row_bytes = ((width * 2 + 31) // 32) * 4
    pixel_bytes = row_bytes * height
    palette_bytes = bytes(
        value
        for level in (0, 85, 170, 255)
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

    px = image.load()
    rows = bytearray()
    for y in range(height):
        row = bytearray(row_bytes)
        out_index = 0
        shift = 6
        current = 0
        for x in range(width):
            current |= (int(px[x, y]) & 0x03) << shift
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


def cover_jpeg_from_bmp(bmp_data: bytes) -> bytes:
    Image = import_pillow_image()

    def u16(offset: int) -> int:
        return int.from_bytes(bmp_data[offset : offset + 2], "little")

    def u32(offset: int) -> int:
        return int.from_bytes(bmp_data[offset : offset + 4], "little")

    def s32(offset: int) -> int:
        return int.from_bytes(bmp_data[offset : offset + 4], "little", signed=True)

    if len(bmp_data) < 70 or bmp_data[:2] != b"BM":
        raise ValueError("Expected a 2-bit BMP cover source.")

    pixel_offset = u32(10)
    width = s32(18)
    raw_height = s32(22)
    height = abs(raw_height)
    bpp = u16(28)
    compression = u32(30)
    if width <= 0 or height <= 0 or bpp != 2 or compression != 0:
        raise ValueError("Expected an uncompressed 2-bit BMP cover source.")

    palette_luma = []
    for index in range(4):
        start = 54 + index * 4
        b, g, r = bmp_data[start], bmp_data[start + 1], bmp_data[start + 2]
        palette_luma.append(int(round(0.299 * r + 0.587 * g + 0.114 * b)))

    row_bytes = ((width * 2 + 31) // 32) * 4
    top_down = raw_height < 0
    pixels = bytearray(width * height)
    for out_y in range(height):
        stored_y = out_y if top_down else height - 1 - out_y
        row_start = pixel_offset + stored_y * row_bytes
        row = bmp_data[row_start : row_start + row_bytes]
        for x in range(width):
            index = (row[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03
            pixels[out_y * width + x] = palette_luma[index]

    cover = Image.frombytes("L", (width, height), bytes(pixels))
    out = io.BytesIO()
    cover.convert("RGB").save(out, format="JPEG", quality=92, optimize=True)
    return out.getvalue()


def bw_page_image_bytes(
    data: bytes,
    margin: int,
    threshold: int,
    coverage: float,
    width: int,
    height: int,
) -> tuple[bytes, tuple[int, int, int, int]]:
    Image = import_pillow_image()
    from io import BytesIO

    with Image.open(BytesIO(data)) as source:
        original_size = source.size
        return bw_page_from_image(source, original_size, margin, threshold, coverage, width, height)


def bw_guided_crop_image_bytes(
    data: bytes,
    crop: GuidedCrop,
    margin: int,
    threshold: int,
    coverage: float,
    width: int,
    height: int,
) -> tuple[bytes, tuple[int, int, int, int]] | None:
    Image = import_pillow_image()
    from io import BytesIO

    with Image.open(BytesIO(data)) as source:
        original_size = source.size
        crop_box = page_box_for_guided_crop(source.width, source.height, crop)
        if crop_box is None:
            return None
        cropped = source.crop(crop_box)
        return bw_page_from_image(cropped, original_size, margin, threshold, coverage, width, height)


def section_xhtml(section_num: int, image_names: list[str], start_page: int, width: int, height: int) -> str:
    image_tags = []
    for offset, image_name in enumerate(image_names):
        page_num = start_page + offset
        safe_src = html.escape(f"../images/{image_name}", quote=True)
        image_tags.append(
            f'<img src="{safe_src}" alt="Page {page_num}" width="{width}" height="{height}" '
            f'style="display:block; width:{width}px; height:{height}px; margin:0; padding:0; '
            'filter:none !important; -webkit-filter:none !important; mix-blend-mode:normal !important;"/>'
        )
    return f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
  <head>
    <title>Section {section_num}</title>
    <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
    <meta name="viewport" content="width={width}, height={height}"/>
  </head>
  <body style="margin:0; padding:0; background:#000; color-scheme:only light;">{''.join(image_tags)}</body>
</html>
"""


def clean_container_xml(opf_path: str) -> str:
    safe_path = html.escape(opf_path, quote=True)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="{safe_path}" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""


def clean_toc_ncx(title: str, section_count: int) -> str:
    nav_points = []
    for index in range(1, section_count + 1):
        nav_points.append(
            f"""    <navPoint id="navPoint-{index}" playOrder="{index}">
      <navLabel><text>Section {index}</text></navLabel>
      <content src="text/section{index:04d}.xhtml"/>
    </navPoint>"""
        )
    safe_title = html.escape(title or "Converted Book")
    return f"""<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="inx-image-pages"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>{safe_title}</text></docTitle>
  <navMap>
{chr(10).join(nav_points)}
  </navMap>
</ncx>
"""


def title_from_metadata(metadata_xml: str) -> str:
    match = re.search(r"<(?:\w+:)?title\b[^>]*>(.*?)</(?:\w+:)?title>", metadata_xml, flags=re.DOTALL)
    if not match:
        return "Converted Book"
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", match.group(1))).strip() or "Converted Book"


def clean_content_opf(metadata_xml: str, section_count: int) -> str:
    manifest_items = [
        '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
        '    <item id="cover" href="images/cover.jpg" media-type="image/jpeg"/>',
    ]
    spine_items = []
    for index in range(1, section_count + 1):
        manifest_items.append(
            f'    <item id="section{index:04d}" href="text/section{index:04d}.xhtml" media-type="application/xhtml+xml"/>'
        )
        spine_items.append(f'    <itemref idref="section{index:04d}"/>')

    return f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
  {metadata_xml}
  <manifest>
{chr(10).join(manifest_items)}
  </manifest>
  <spine toc="ncx" page-progression-direction="ltr">
{chr(10).join(spine_items)}
  </spine>
</package>
"""


def chunked(items: list[tuple[str, bytes, tuple[int, int, int, int]]], size: int):
    for start in range(0, len(items), size):
        yield start, items[start : start + size]


def rebuild_epub_as_bw_image_pages(
    epub_path: Path,
    margin: int,
    threshold: int,
    coverage: float,
    width: int,
    height: int,
    include_guided_crops: bool,
    pages_per_section: int,
) -> int:
    epub_path = epub_path.resolve()
    temp_path = epub_path.with_suffix(epub_path.suffix + ".tmp")
    opf_path = "content.opf"

    with zipfile.ZipFile(epub_path, "r") as source:
        source_opf_path = opf_path_from_container(source)
        metadata_xml, page_entries = ordered_image_pages_from_epub(source, source_opf_path, include_guided_crops)
        if not page_entries:
            raise SystemExit("No image pages found to optimize.")

        title = title_from_metadata(metadata_xml)
        processed: list[tuple[str, bytes, tuple[int, int, int, int]]] = []
        skipped_crops = 0
        for image_path, crop in page_entries:
            data = source.read(image_path)
            if crop is None:
                result = bw_page_image_bytes(data, margin, threshold, coverage, width, height)
            else:
                result = bw_guided_crop_image_bytes(data, crop, margin, threshold, coverage, width, height)
                if result is None:
                    skipped_crops += 1
                    continue
            out_data, dims = result
            index = len(processed) + 1
            image_name = f"page{index:04d}.bmp"
            processed.append((image_name, out_data, dims))
            old_w, old_h, new_w, new_h = dims
            kind = "crop" if crop is not None else "page"
            print(f"BW {kind} {index}: {image_path} {old_w}x{old_h} -> {new_w}x{new_h} on {width}x{height}", flush=True)
        if skipped_crops:
            print(f"Skipped {skipped_crops} near-full/invalid guided crop(s).", flush=True)

    with zipfile.ZipFile(temp_path, "w") as dest:
        sections = list(chunked(processed, max(1, pages_per_section)))
        dest.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        dest.writestr("META-INF/container.xml", clean_container_xml(opf_path), compress_type=zipfile.ZIP_DEFLATED)
        dest.writestr("content.opf", clean_content_opf(metadata_xml, len(sections)), compress_type=zipfile.ZIP_DEFLATED)
        dest.writestr("toc.ncx", clean_toc_ncx(title, len(sections)), compress_type=zipfile.ZIP_DEFLATED)
        dest.writestr("images/cover.jpg", cover_jpeg_from_bmp(processed[0][1]), compress_type=zipfile.ZIP_DEFLATED)
        for index, (image_name, image_data, _dims) in enumerate(processed, start=1):
            dest.writestr(f"images/{image_name}", image_data, compress_type=zipfile.ZIP_DEFLATED)
        for section_index, (start, section_pages) in enumerate(sections, start=1):
            dest.writestr(
                f"text/section{section_index:04d}.xhtml",
                section_xhtml(
                    section_index,
                    [image_name for image_name, _image_data, _dims in section_pages],
                    start + 1,
                    width,
                    height,
                ),
                compress_type=zipfile.ZIP_DEFLATED,
            )

    temp_path.replace(epub_path)
    print(f"BW EPUB packaging: {len(processed)} image page(s) in {len(sections)} section(s).", flush=True)
    return len(processed)


def rebuild_bw_with_best_runtime(
    epub_path: Path,
    margin: int,
    threshold: int,
    coverage: float,
    width: int,
    height: int,
    include_guided_crops: bool,
    pages_per_section: int,
) -> int | None:
    try:
        import_pillow_image()
        return rebuild_epub_as_bw_image_pages(
            epub_path,
            margin,
            threshold,
            coverage,
            width,
            height,
            include_guided_crops,
            pages_per_section,
        )
    except RuntimeError:
        calibre_debug = find_calibre_debug()
        if not calibre_debug:
            raise SystemExit("BW optimization needs Pillow or calibre-debug.")

        command = [
            calibre_debug,
            "-e",
            str(Path(__file__).resolve()),
            "--",
            "--bw-epub-only",
            str(epub_path),
            "--crop-margin",
            str(margin),
            "--crop-black-threshold",
            str(threshold),
            "--crop-edge-coverage",
            str(coverage),
            "--bw-width",
            str(width),
            "--bw-height",
            str(height),
            "--bw-pages-per-section",
            str(pages_per_section),
        ]
        if include_guided_crops:
            command.append("--guided-crops")
        result = subprocess.run(command, check=False, env=calibre_env())
        if result.returncode != 0:
            raise SystemExit("BW optimization failed after conversion.")
        return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert AZW3/MOBI/KF8 books to EPUB with Calibre.",
    )
    parser.add_argument(
        "--crop-epub-only",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--bw-epub-only",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "input",
        nargs="?",
        help="Optional input path. If omitted, a file picker opens.",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Optional output EPUB path or destination folder.",
    )
    parser.add_argument(
        "-f",
        "--overwrite",
        action="store_true",
        help="Overwrite the output EPUB if it already exists.",
    )
    parser.add_argument(
        "--no-crop-images",
        action="store_true",
        help="Do not trim near-black borders from images after conversion.",
    )
    parser.add_argument(
        "--crop-margin",
        type=int,
        default=DEFAULT_CROP_MARGIN,
        help=f"Pixels of border to keep after image cropping. Default: {DEFAULT_CROP_MARGIN}.",
    )
    parser.add_argument(
        "--crop-black-threshold",
        type=int,
        default=DEFAULT_BLACK_THRESHOLD,
        help=f"Maximum RGB channel value treated as black. Default: {DEFAULT_BLACK_THRESHOLD}.",
    )
    parser.add_argument(
        "--crop-edge-coverage",
        type=float,
        default=DEFAULT_EDGE_COVERAGE,
        help=f"Required black-pixel share for an edge row/column. Default: {DEFAULT_EDGE_COVERAGE}.",
    )
    parser.add_argument(
        "--jpeg-quality",
        type=int,
        default=DEFAULT_JPEG_QUALITY,
        help=f"JPEG quality for cropped images. Default: {DEFAULT_JPEG_QUALITY}.",
    )
    parser.add_argument(
        "--bw",
        action="store_true",
        help=(
            "Optimize image-heavy books for the e-reader: one "
            f"{DEFAULT_BW_WIDTH}x{DEFAULT_BW_HEIGHT} dithered grayscale image page per spine item."
        ),
    )
    parser.add_argument(
        "--bw-width",
        type=int,
        default=DEFAULT_BW_WIDTH,
        help=f"BW output page width. Default: {DEFAULT_BW_WIDTH}.",
    )
    parser.add_argument(
        "--bw-height",
        type=int,
        default=DEFAULT_BW_HEIGHT,
        help=f"BW output page height. Default: {DEFAULT_BW_HEIGHT}.",
    )
    parser.add_argument(
        "--bw-pages-per-section",
        type=int,
        default=DEFAULT_BW_PAGES_PER_SECTION,
        help=(
            "Generated image pages to group into each EPUB spine section. "
            f"Default: {DEFAULT_BW_PAGES_PER_SECTION}."
        ),
    )
    parser.add_argument(
        "--guided-crops",
        action="store_true",
        help="With --bw, add Kindle/Comixology magnify regions as real cropped pages after each full page.",
    )
    parser.add_argument(
        "--fresh-name",
        action="store_true",
        help="Write a timestamped EPUB filename so the X3 does not reuse same-path cached pages.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.crop_epub_only:
        epub_path = Path(args.crop_epub_only).expanduser().resolve()
        scanned, changed = crop_epub_images(
            epub_path,
            max(0, args.crop_margin),
            max(0, min(255, args.crop_black_threshold)),
            max(0.5, min(1.0, args.crop_edge_coverage)),
            max(1, min(100, args.jpeg_quality)),
        )
        print(f"Image crop pass: {changed}/{scanned} image(s) cropped.", flush=True)
        return 0

    if args.bw_epub_only:
        epub_path = Path(args.bw_epub_only).expanduser().resolve()
        page_count = rebuild_epub_as_bw_image_pages(
            epub_path,
            max(0, args.crop_margin),
            max(0, min(255, args.crop_black_threshold)),
            max(0.5, min(1.0, args.crop_edge_coverage)),
            max(1, args.bw_width),
            max(1, args.bw_height),
            args.guided_crops,
            max(1, args.bw_pages_per_section),
        )
        print(f"BW image-page rebuild: {page_count} page(s).", flush=True)
        return 0

    input_path = Path(args.input).expanduser() if args.input else pick_input_file()
    input_path = input_path.resolve()
    validate_input(input_path)

    ebook_convert = find_ebook_convert()
    if not ebook_convert:
        raise SystemExit(
            "Calibre ebook-convert was not found. Install Calibre, or set "
            "EBOOK_CONVERT=/path/to/ebook-convert."
        )

    output_path = choose_output_path(input_path, args.output, args.overwrite).resolve()
    if args.fresh_name:
        output_path = add_fresh_name_suffix(output_path).resolve()
    print(f"Converting: {input_path}", flush=True)
    print(f"Writing:    {output_path}", flush=True)
    convert(input_path, output_path, ebook_convert)
    if args.bw:
        print("Rebuilding as BW image pages...", flush=True)
        page_count = rebuild_bw_with_best_runtime(
            output_path,
            max(0, args.crop_margin),
            max(0, min(255, args.crop_black_threshold)),
            max(0.5, min(1.0, args.crop_edge_coverage)),
            max(1, args.bw_width),
            max(1, args.bw_height),
            args.guided_crops,
            max(1, args.bw_pages_per_section),
        )
        if page_count is not None:
            print(f"BW image-page rebuild: {page_count} page(s).", flush=True)
    elif not args.no_crop_images:
        print("Cropping near-black image borders...", flush=True)
        result = crop_epub_images_with_best_runtime(
            output_path,
            max(0, args.crop_margin),
            max(0, min(255, args.crop_black_threshold)),
            max(0.5, min(1.0, args.crop_edge_coverage)),
            max(1, min(100, args.jpeg_quality)),
        )
        if result is not None:
            scanned, changed = result
            print(f"Image crop pass: {changed}/{scanned} image(s) cropped.", flush=True)
    print(f"Done: {output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
