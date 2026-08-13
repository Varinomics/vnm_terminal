#!/usr/bin/env python3
"""Render deterministic Qt IFW PNGs from the canonical brand sources."""

from __future__ import annotations

import argparse
from pathlib import Path

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QGuiApplication, QImage, QImageReader, QPainter
from PyQt6.QtSvg import QSvgRenderer


BANNER_SIZE = (998, 80)
BANNER_LOGO_SIZE = (300, 42)
BANNER_MARGIN = 16
BANNER_LOGO_TOP = 11
BANNER_TEXT_SAFE_RIGHT = 650
BANNER_TEXT_LOGO_GUTTER = 32
GEOMETRY_SIZE = (300, 174)
BACKGROUND = QColor("#111111")


def render_banner(logo_path: Path, destination: Path, scale: int) -> None:
    width, height = (dimension * scale for dimension in BANNER_SIZE)
    logo_width, logo_height = (
        dimension * scale for dimension in BANNER_LOGO_SIZE
    )
    logo = QImage(str(logo_path))
    if logo.isNull():
        raise RuntimeError(f"Qt could not load the canonical logo: {logo_path}")

    image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(BACKGROUND)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
    logo = logo.scaled(
        logo_width,
        logo_height,
        Qt.AspectRatioMode.KeepAspectRatio,
        Qt.TransformationMode.SmoothTransformation,
    )
    x_position = width - BANNER_MARGIN * scale - logo.width()
    y_position = BANNER_LOGO_TOP * scale
    if BANNER_TEXT_SAFE_RIGHT + BANNER_TEXT_LOGO_GUTTER > x_position // scale:
        raise RuntimeError("The Banner logo violates the text-safe region")
    painter.drawImage(x_position, y_position, logo)
    painter.end()
    if not image.save(str(destination), "PNG", 100):
        raise RuntimeError(f"Qt could not write the banner: {destination}")


def render_geometry(svg_path: Path, destination: Path, scale: int) -> None:
    renderer = QSvgRenderer(str(svg_path))
    if not renderer.isValid():
        raise RuntimeError(f"Qt could not load the brand geometry: {svg_path}")

    width, height = (dimension * scale for dimension in GEOMETRY_SIZE)
    image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(BACKGROUND)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
    renderer.render(painter, QRectF(0.0, 0.0, float(width), float(height)))
    painter.end()
    if not image.save(str(destination), "PNG", 100):
        raise RuntimeError(f"Qt could not write the geometry: {destination}")


def verify_banner_logo_bounds(path: Path, scale: int) -> None:
    image = QImage(str(path)).convertToFormat(QImage.Format.Format_RGBA8888)
    background = bytes((17, 17, 17, 255))
    pixels = image.constBits().asstring(image.sizeInBytes())
    left = image.width()
    top = image.height()
    right = 0
    bottom = 0
    for y_position in range(image.height()):
        row_offset = y_position * image.bytesPerLine()
        for x_position in range(image.width()):
            pixel_offset = row_offset + x_position * 4
            if pixels[pixel_offset : pixel_offset + 4] != background:
                left = min(left, x_position)
                top = min(top, y_position)
                right = max(right, x_position + 1)
                bottom = max(bottom, y_position + 1)

    if right == 0:
        raise RuntimeError(f"The generated Banner has no wordmark: {path}")
    actual_bounds = (left, top, right, bottom)
    expected_bounds = tuple(
        coordinate * scale for coordinate in (682, 11, 982, 53)
    )
    if actual_bounds != expected_bounds:
        raise RuntimeError(
            f"Unexpected wordmark bounds for {path}: {actual_bounds} instead of "
            f"{expected_bounds}"
        )


def verify_png(path: Path, expected_size: tuple[int, int]) -> None:
    reader = QImageReader(str(path), b"PNG")
    if not reader.canRead():
        raise RuntimeError(f"Qt cannot read generated PNG {path}: {reader.errorString()}")
    if (reader.size().width(), reader.size().height()) != expected_size:
        raise RuntimeError(
            f"Unexpected dimensions for {path}: {reader.size().width()}x"
            f"{reader.size().height()} instead of {expected_size[0]}x"
            f"{expected_size[1]}"
        )
    if reader.read().isNull():
        raise RuntimeError(f"Qt failed to decode generated PNG: {path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    arguments = parser.parse_args()

    source_root = arguments.source_root.resolve()
    ifw_root = source_root / "packaging" / "windows" / "ifw"
    logo_path = ifw_root / "varinomics_logo.png"
    svg_path = ifw_root / "varinomics_geometry.svg"

    application = QGuiApplication([])
    application.setApplicationName("vnm-terminal-ifw-brand-renderer")
    for scale, suffix in ((1, ""), (2, "@2x")):
        banner_path = ifw_root / f"varinomics_banner{suffix}.png"
        geometry_path = ifw_root / f"varinomics_geometry{suffix}.png"
        render_banner(logo_path, banner_path, scale)
        render_geometry(svg_path, geometry_path, scale)
        verify_png(
            banner_path,
            tuple(dimension * scale for dimension in BANNER_SIZE),
        )
        verify_banner_logo_bounds(banner_path, scale)
        verify_png(
            geometry_path,
            tuple(dimension * scale for dimension in GEOMETRY_SIZE),
        )


if __name__ == "__main__":
    main()
