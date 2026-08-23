#!/usr/bin/env python3
"""Compute temporal stability gates from benchmark Radiance-HDR sequences.

Requires NumPy. The benchmark's `--capture-every` output is discovered through
the artifact manifest when available. Surface CV is restricted to pixels whose
SurfaceGuides `surface_motion_hit` alpha channel stores primaryHitT: zero is a
miss and every positive value is a primary-surface hit.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import re
import struct
import sys
from pathlib import Path
from typing import Any, Optional

import numpy as np


_RESOLUTION = re.compile(rb"^([+-])Y\s+(\d+)\s+([+-])X\s+(\d+)\s*$")
_DDS_MAGIC = b"DDS "
_DDS_HEADER_SIZE = 124
_DDS_PIXEL_FORMAT_SIZE = 32
_DDSD_CAPS = 0x1
_DDSD_HEIGHT = 0x2
_DDSD_WIDTH = 0x4
_DDSD_PITCH = 0x8
_DDSD_PIXELFORMAT = 0x1000
_DDPF_FOURCC = 0x4
_D3DFMT_A16B16G16R16F = 113
_FOURCC_DX10 = int.from_bytes(b"DX10", "little")
_DXGI_FORMAT_R16G16B16A16_FLOAT = 10
_D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
_DDS_RESOURCE_MISC_TEXTURECUBE = 0x4
_DDSCAPS2_CUBEMAP = 0x200
_DDSCAPS2_VOLUME = 0x200000


@dataclass(frozen=True)
class SurfaceHitMaskSelection:
    paths: tuple[Path, ...]
    source: str
    combination: str
    per_frame_available: int


def _read_byte(stream) -> int:
    value = stream.read(1)
    if len(value) != 1:
        raise ValueError("Unexpected end of Radiance HDR data")
    return value[0]


def load_radiance_hdr(path: Path) -> np.ndarray:
    """Load the adaptive RLE/raw RGBE rows written by DirectXTex."""
    with path.open("rb") as stream:
        if stream.readline().rstrip() not in (b"#?RADIANCE", b"#?RGBE"):
            raise ValueError(f"Not a Radiance HDR file: {path}")
        format_seen = False
        while True:
            line = stream.readline()
            if not line:
                raise ValueError(f"HDR header is truncated: {path}")
            stripped = line.strip()
            if not stripped:
                break
            if stripped == b"FORMAT=32-bit_rle_rgbe":
                format_seen = True
        if not format_seen:
            raise ValueError(f"Unsupported HDR encoding: {path}")

        resolution_line = stream.readline().strip()
        match = _RESOLUTION.match(resolution_line)
        if not match:
            raise ValueError(f"Unsupported HDR orientation: {resolution_line!r}")
        y_sign, height_text, x_sign, width_text = match.groups()
        height = int(height_text)
        width = int(width_text)
        if width < 8 or width > 32767 or height <= 0:
            raise ValueError(f"Unsupported HDR dimensions {width}x{height}: {path}")

        rgbe = np.empty((height, width, 4), dtype=np.uint8)
        for y in range(height):
            scanline_header = stream.read(4)
            expected = bytes((2, 2, (width >> 8) & 0xFF, width & 0xFF))
            if scanline_header != expected:
                # DirectXTex's adaptive encoder falls back to a raw RGBE row
                # whenever its RLE representation would not be smaller. This
                # decision is made independently for every scanline, so an HDR
                # file can legitimately mix compressed and raw rows.
                remaining = stream.read((width - 1) * 4)
                if len(remaining) != (width - 1) * 4:
                    raise ValueError(f"Truncated raw HDR scanline {y}: {path}")
                rgbe[y] = np.frombuffer(
                    scanline_header + remaining,
                    dtype=np.uint8).reshape(width, 4)
                continue
            channels = np.empty((4, width), dtype=np.uint8)
            for channel in range(4):
                x = 0
                while x < width:
                    code = _read_byte(stream)
                    if code > 128:
                        count = code - 128
                        if count == 0 or x + count > width:
                            raise ValueError(f"Invalid HDR RLE run at row {y}: {path}")
                        channels[channel, x:x + count] = _read_byte(stream)
                    else:
                        count = code
                        if count == 0 or x + count > width:
                            raise ValueError(f"Invalid HDR literal run at row {y}: {path}")
                        values = stream.read(count)
                        if len(values) != count:
                            raise ValueError(f"Truncated HDR literal run at row {y}: {path}")
                        channels[channel, x:x + count] = np.frombuffer(values, dtype=np.uint8)
                    x += count
            rgbe[y] = channels.T

    rgb = rgbe[..., :3].astype(np.float32)
    exponent = rgbe[..., 3].astype(np.int32)
    nonzero = exponent != 0
    scale = np.zeros((height, width), dtype=np.float32)
    scale[nonzero] = np.ldexp(np.ones(np.count_nonzero(nonzero), dtype=np.float32), exponent[nonzero] - 136)
    image = rgb * scale[..., None]
    if y_sign == b"+":
        image = image[::-1]
    if x_sign == b"-":
        image = image[:, ::-1]
    return image


def load_dds_rgba16_float(path: Path) -> np.ndarray:
    """Load the top mip of a DirectXTex R16G16B16A16_FLOAT DDS.

    DirectXTex normally writes this format with the legacy D3D9 FourCC 113,
    but the DX10 extended header is accepted too. Rows may be pitch-padded.
    """
    with path.open("rb") as stream:
        prefix = stream.read(4 + _DDS_HEADER_SIZE)
        if len(prefix) != 4 + _DDS_HEADER_SIZE or prefix[:4] != _DDS_MAGIC:
            raise ValueError(f"Not a complete DDS file: {path}")

        header = prefix[4:]
        header_size, flags, height, width, pitch_or_linear_size, depth, _ = struct.unpack_from(
            "<7I", header, 0)
        if header_size != _DDS_HEADER_SIZE:
            raise ValueError(f"Unsupported DDS header size {header_size}: {path}")
        required_flags = _DDSD_CAPS | _DDSD_HEIGHT | _DDSD_WIDTH | _DDSD_PIXELFORMAT
        if (flags & required_flags) != required_flags or width == 0 or height == 0:
            raise ValueError(f"DDS is missing required 2D texture metadata: {path}")
        if depth not in (0, 1):
            raise ValueError(f"DDS volume textures are not supported: {path}")
        caps2 = struct.unpack_from("<I", header, 108)[0]
        if caps2 & (_DDSCAPS2_CUBEMAP | _DDSCAPS2_VOLUME):
            raise ValueError(f"DDS cubemap/volume resources are not supported: {path}")

        pixel_format_size, pixel_format_flags, four_cc = struct.unpack_from("<3I", header, 72)
        if pixel_format_size != _DDS_PIXEL_FORMAT_SIZE or not (pixel_format_flags & _DDPF_FOURCC):
            raise ValueError(f"DDS is not an R16G16B16A16_FLOAT FourCC texture: {path}")

        data_offset = 4 + _DDS_HEADER_SIZE
        if four_cc == _FOURCC_DX10:
            extension = stream.read(20)
            if len(extension) != 20:
                raise ValueError(f"DDS DX10 header is truncated: {path}")
            dxgi_format, resource_dimension, misc_flag, array_size, _ = struct.unpack("<5I", extension)
            if dxgi_format != _DXGI_FORMAT_R16G16B16A16_FLOAT:
                raise ValueError(f"Unsupported DDS DXGI format {dxgi_format}: {path}")
            if (resource_dimension != _D3D10_RESOURCE_DIMENSION_TEXTURE2D or
                    array_size != 1 or (misc_flag & _DDS_RESOURCE_MISC_TEXTURECUBE)):
                raise ValueError(f"DDS must contain one non-cubemap 2D texture: {path}")
            data_offset += 20
        elif four_cc != _D3DFMT_A16B16G16R16F:
            raise ValueError(f"Unsupported DDS FourCC {four_cc}: {path}")

        tight_row_pitch = width * 4 * np.dtype("<f2").itemsize
        row_pitch = pitch_or_linear_size if flags & _DDSD_PITCH else tight_row_pitch
        if row_pitch < tight_row_pitch:
            raise ValueError(
                f"DDS row pitch {row_pitch} is smaller than {tight_row_pitch}: {path}")
        expected_bytes = row_pitch * height
        available_bytes = path.stat().st_size - data_offset
        if expected_bytes > available_bytes:
            raise ValueError(
                f"DDS top mip is truncated (wanted {expected_bytes}, found {available_bytes} bytes): {path}")
        pixels = stream.read(expected_bytes)
        if len(pixels) != expected_bytes:
            raise ValueError(
                f"DDS top mip is truncated (wanted {expected_bytes} bytes): {path}")

    if row_pitch == tight_row_pitch:
        return np.frombuffer(pixels, dtype="<f2").reshape(height, width, 4).copy()

    image = np.empty((height, width, 4), dtype=np.float16)
    for y in range(height):
        offset = y * row_pitch
        image[y] = np.frombuffer(
            pixels, dtype="<f2", count=width * 4, offset=offset).reshape(width, 4)
    return image


def _load_manifest(directory: Path) -> Optional[dict[str, Any]]:
    manifest = directory / "artifacts.json"
    if not manifest.is_file():
        manifest = directory / "artifact-manifest.json"
    if not manifest.is_file():
        return None
    document = json.loads(manifest.read_text(encoding="utf-8-sig"))
    if not isinstance(document, dict) or not isinstance(document.get("artifacts", []), list):
        raise ValueError(f"Artifact manifest has an invalid schema: {manifest}")
    return document


def _artifact_path(directory: Path, artifact: dict[str, Any]) -> Optional[Path]:
    relative = artifact.get("path") or artifact.get("relativePath")
    if not isinstance(relative, str) or not relative:
        return None
    root = directory.resolve()
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exception:
        raise ValueError(f"Artifact path leaves the benchmark directory: {relative}") from exception
    return candidate


def discover_hdr_sequence(directory: Path) -> list[Path]:
    # `artifacts.json` is the current harness contract. Accept the early
    # development spelling as well so older captures remain analyzable.
    candidates: list[Path] = []
    document = _load_manifest(directory)
    if document is not None:
        for artifact in document.get("artifacts", []):
            if not isinstance(artifact, dict):
                continue
            role = str(artifact.get("role", "")).lower()
            phase = str(artifact.get("phase", "")).lower()
            candidate = _artifact_path(directory, artifact)
            if candidate and role in ("final_hdr", "beauty_hdr", "hdr") and phase != "final":
                candidates.append(candidate)
    if not candidates:
        candidates = [
            path
            for pattern in ("beauty_hdr.hdr", "final_hdr.hdr")
            for path in directory.rglob(pattern)
            if path.parent != directory
        ]
    return sorted(set(path.resolve() for path in candidates if path.is_file()))


def discover_surface_hit_masks(
        directory: Path, selected_hdr: list[Path]) -> SurfaceHitMaskSelection:
    """Find masks aligned to selected HDRs, preferring measured-frame AOVs."""
    selected = [path.resolve() for path in selected_hdr]
    selected_set = set(selected)
    document = _load_manifest(directory)
    per_frame_by_hdr: dict[Path, Path] = {}
    final_candidates: list[Path] = []

    if document is not None:
        hdr_by_measured_frame: dict[int, Path] = {}
        hit_by_measured_frame: dict[int, Path] = {}
        for artifact in document.get("artifacts", []):
            if not isinstance(artifact, dict):
                continue
            role = str(artifact.get("role", "")).lower()
            phase = str(artifact.get("phase", "")).lower()
            candidate = _artifact_path(directory, artifact)
            if candidate is None:
                continue
            if role in ("final_hdr", "beauty_hdr", "hdr") and phase != "final":
                measured = artifact.get("measuredFrameIndex")
                if isinstance(measured, int):
                    hdr_by_measured_frame[measured] = candidate
            elif role == "surface_motion_hit":
                source_format = artifact.get("sourceFormat")
                if source_format and str(source_format).upper() != "R16G16B16A16_FLOAT":
                    raise ValueError(
                        f"surface_motion_hit has unsupported source format {source_format}: {candidate}")
                if phase == "final":
                    final_candidates.append(candidate)
                else:
                    measured = artifact.get("measuredFrameIndex")
                    if isinstance(measured, int):
                        hit_by_measured_frame[measured] = candidate

        for measured, hdr_path in hdr_by_measured_frame.items():
            if hdr_path.resolve() in selected_set and measured in hit_by_measured_frame:
                per_frame_by_hdr[hdr_path.resolve()] = hit_by_measured_frame[measured]

    # Keep old captures without a manifest useful when the AOV uses the current
    # frame-directory naming contract.
    for hdr_path in selected:
        sibling = hdr_path.parent / "surface_motion_hit.dds"
        if hdr_path not in per_frame_by_hdr and sibling.is_file():
            per_frame_by_hdr[hdr_path] = sibling.resolve()

    per_frame_available = sum(path in per_frame_by_hdr for path in selected)
    if per_frame_available:
        if per_frame_available != len(selected):
            raise ValueError(
                "Per-frame surface_motion_hit artifacts are incomplete for the selected HDR sequence "
                f"({per_frame_available}/{len(selected)})")
        paths = tuple(per_frame_by_hdr[path] for path in selected)
        if not all(path.is_file() for path in paths):
            raise ValueError("A registered per-frame surface_motion_hit artifact is missing")
        return SurfaceHitMaskSelection(
            paths=paths,
            source="perFrameSurfaceGuidesArtifacts",
            combination="intersectionAcrossAnalyzedFrames",
            per_frame_available=per_frame_available)

    final_candidates.append((directory / "surface_motion_hit.dds").resolve())
    final_path = next((path for path in final_candidates if path.is_file()), None)
    if final_path is None:
        raise ValueError(
            "No SurfaceGuides hit mask was found. Capture per-frame AOVs with --capture-aovs "
            "or retain the final surface_motion_hit.dds artifact.")
    return SurfaceHitMaskSelection(
        paths=(final_path,),
        source="finalStaticCameraSurfaceGuidesArtifact",
        combination="singleFinalStaticCameraMask",
        per_frame_available=0)


def percentile(values: np.ndarray, amount: float) -> float:
    return float(np.percentile(values, amount)) if values.size else math.nan


def _luminance(image: np.ndarray) -> np.ndarray:
    return image[..., 0] * 0.2126 + image[..., 1] * 0.7152 + image[..., 2] * 0.0722


def _bilinear(image: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    height, width = image.shape
    x = np.clip(x, 0.0, width - 1.001)
    y = np.clip(y, 0.0, height - 1.001)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, width - 1)
    y1 = np.minimum(y0 + 1, height - 1)
    fx = x - x0
    fy = y - y0
    return ((1.0 - fx) * (1.0 - fy) * image[y0, x0] +
            fx * (1.0 - fy) * image[y0, x1] +
            (1.0 - fx) * fy * image[y1, x0] +
            fx * fy * image[y1, x1])


def edge_width_statistics(interactive: np.ndarray, reference: np.ndarray) -> dict:
    if interactive.shape != reference.shape:
        raise ValueError("Interactive and reference HDR dimensions differ")
    reference_luma = _luminance(np.where(np.isfinite(reference), reference, 0.0)).astype(np.float32)
    interactive_luma = _luminance(np.where(np.isfinite(interactive), interactive, 0.0)).astype(np.float32)
    gx = np.zeros_like(reference_luma)
    gy = np.zeros_like(reference_luma)
    gx[:, 1:-1] = (reference_luma[:, 2:] - reference_luma[:, :-2]) * 0.5
    gy[1:-1, :] = (reference_luma[2:, :] - reference_luma[:-2, :]) * 0.5
    magnitude = np.hypot(gx, gy)
    finite_magnitude = magnitude[np.isfinite(magnitude)]
    if finite_magnitude.size == 0:
        raise ValueError("Reference HDR has no finite edge gradients")
    threshold = float(np.percentile(finite_magnitude, 97.5))
    border = 7
    candidate_mask = magnitude > max(threshold, 1.0e-5)
    candidate_mask[:border] = False
    candidate_mask[-border:] = False
    candidate_mask[:, :border] = False
    candidate_mask[:, -border:] = False
    ys, xs = np.nonzero(candidate_mask)
    if xs.size < 32:
        raise ValueError("Reference HDR has too few strong edges for edge-width analysis")
    if xs.size > 20000:
        stride = int(math.ceil(xs.size / 20000.0))
        xs = xs[::stride]
        ys = ys[::stride]
    normal_x = gx[ys, xs]
    normal_y = gy[ys, xs]
    normal_length = np.maximum(np.hypot(normal_x, normal_y), 1.0e-8)
    normal_x /= normal_length
    normal_y /= normal_length

    offsets = np.arange(-6.0, 6.001, 0.25, dtype=np.float32)
    sample_x = xs[:, None].astype(np.float32) + normal_x[:, None] * offsets[None, :]
    sample_y = ys[:, None].astype(np.float32) + normal_y[:, None] * offsets[None, :]
    reference_profile = _bilinear(reference_luma, sample_x, sample_y)
    interactive_profile = _bilinear(interactive_luma, sample_x, sample_y)
    low = np.mean(reference_profile[:, :4], axis=1)
    high = np.mean(reference_profile[:, -4:], axis=1)
    reverse = high < low
    if np.any(reverse):
        reference_profile[reverse] = reference_profile[reverse, ::-1]
        interactive_profile[reverse] = interactive_profile[reverse, ::-1]
        swapped = low[reverse].copy()
        low[reverse] = high[reverse]
        high[reverse] = swapped
    contrast = high - low
    keep = contrast > max(float(np.percentile(np.abs(contrast), 25.0)), 1.0e-4)
    reference_profile = (reference_profile[keep] - low[keep, None]) / contrast[keep, None]
    interactive_profile = (interactive_profile[keep] - low[keep, None]) / contrast[keep, None]
    if reference_profile.shape[0] < 32:
        raise ValueError("Reference HDR has too few high-contrast edge profiles")

    def widths(profile: np.ndarray) -> np.ndarray:
        above_10 = profile >= 0.1
        above_90 = profile >= 0.9
        valid = np.any(above_10, axis=1) & np.any(above_90, axis=1)
        index_10 = np.argmax(above_10, axis=1)
        index_90 = np.argmax(above_90, axis=1)
        result = offsets[index_90] - offsets[index_10]
        result[~valid | (index_90 <= index_10)] = np.nan
        return result

    reference_width = widths(reference_profile)
    interactive_width = widths(interactive_profile)
    valid_width = np.isfinite(reference_width) & np.isfinite(interactive_width) & (reference_width >= 0.25)
    ratios = interactive_width[valid_width] / reference_width[valid_width]
    if ratios.size < 32:
        raise ValueError("Too few valid 10-90% edge-width measurements")
    ratio_median = percentile(ratios, 50.0)
    ratio_p95 = percentile(ratios, 95.0)
    return {
        "sampleCount": int(ratios.size),
        "ratioMedian": ratio_median,
        "ratioP95": ratio_p95,
        "ratioThreshold": 1.15,
        "passed": ratio_p95 <= 1.15,
    }


def _load_surface_hit_mask(
        selection: SurfaceHitMaskSelection,
        dimensions: tuple[int, int]) -> tuple[np.ndarray, dict[str, Any]]:
    width, height = dimensions
    combined: Optional[np.ndarray] = None
    per_artifact_counts: list[int] = []
    for path in selection.paths:
        image = load_dds_rgba16_float(path)
        if image.shape[:2] != (height, width):
            raise ValueError(
                f"Surface hit mask dimensions {image.shape[1]}x{image.shape[0]} do not match "
                f"HDR dimensions {width}x{height}: {path}")
        hit_channel = image[..., 3].astype(np.float32)
        if not np.all(np.isfinite(hit_channel)):
            raise ValueError(f"Surface hit mask alpha contains NaN/Inf: {path}")
        artifact_mask = hit_channel > 0.0
        per_artifact_counts.append(int(np.count_nonzero(artifact_mask)))
        combined = artifact_mask if combined is None else combined & artifact_mask

    if combined is None:
        raise ValueError("Surface hit mask selection is empty")
    return combined, {
        "source": selection.source,
        "combination": selection.combination,
        "artifactCount": len(selection.paths),
        "perFrameArtifactsAvailable": selection.per_frame_available,
        "firstArtifact": str(selection.paths[0]),
        "lastArtifact": str(selection.paths[-1]),
        "hitChannel": "alpha",
        "hitChannelSemantic": "primaryHitT (0=miss, >0=hit)",
        "hitThresholdExclusive": 0.0,
        "requiresStaticCamera": selection.source == "finalStaticCameraSurfaceGuidesArtifact",
        "minimumArtifactSurfacePixels": min(per_artifact_counts),
        "maximumArtifactSurfacePixels": max(per_artifact_counts),
        "intersectionSurfacePixels": int(np.count_nonzero(combined)),
    }


def analyze(
        files: list[Path],
        start: int,
        count: int,
        reference_hdr: Optional[Path],
        hit_mask_selection: SurfaceHitMaskSelection) -> dict:
    selected = files[start:start + count if count > 0 else None]
    if len(selected) < 2:
        raise ValueError("Temporal analysis requires at least two captured HDR frames")

    mean = None
    m2 = None
    nonfinite_values = 0
    nonfinite_pixels = 0
    dimensions = None
    for sample_count, path in enumerate(selected, start=1):
        image = load_radiance_hdr(path)
        if dimensions is None:
            dimensions = (int(image.shape[1]), int(image.shape[0]))
            mean = np.zeros(image.shape[:2], dtype=np.float64)
            m2 = np.zeros_like(mean)
        elif dimensions != (image.shape[1], image.shape[0]):
            raise ValueError(f"Sequence dimension changed at {path}")

        finite = np.isfinite(image)
        invalid_pixel = ~np.all(finite, axis=2)
        nonfinite_values += int(np.size(finite) - np.count_nonzero(finite))
        nonfinite_pixels += int(np.count_nonzero(invalid_pixel))
        image = np.where(finite, image, 0.0)
        luminance = _luminance(image)
        delta = luminance - mean
        mean += delta / float(sample_count)
        m2 += delta * (luminance - mean)

    surface_hit_mask, surface_hit_details = _load_surface_hit_mask(
        hit_mask_selection, dimensions)
    variance = np.maximum(m2 / float(len(selected) - 1), 0.0)
    cv = np.full(mean.shape, np.nan, dtype=np.float64)
    positive_mean = mean > 0.0
    # CV is sigma / |mean|. A zero-mean signal has undefined CV and is
    # reported separately; no luminance floor is used to suppress dark hits.
    np.divide(np.sqrt(variance), np.abs(mean), out=cv, where=positive_mean)
    valid = surface_hit_mask & positive_mean & np.isfinite(cv)
    cv_values = cv[valid]
    cv_median = percentile(cv_values, 50.0)
    cv_p95 = percentile(cv_values, 95.0)
    gate = nonfinite_values == 0 and cv_values.size > 0 and cv_median <= 0.01 and cv_p95 <= 0.03
    result = {
        "schemaVersion": 2,
        "inputFrames": len(files),
        "analyzedFrames": len(selected),
        "startFrame": start,
        "firstArtifact": str(selected[0]),
        "lastArtifact": str(selected[-1]),
        "width": dimensions[0],
        "height": dimensions[1],
        "validLuminancePixels": int(cv_values.size),
        "surfaceHitMask": surface_hit_details,
        "nonFiniteValues": nonfinite_values,
        "nonFinitePixelsAcrossFrames": nonfinite_pixels,
        "surfaceLuminanceTemporalCv": {
            "median": cv_median,
            "p95": cv_p95,
            "medianThreshold": 0.01,
            "p95Threshold": 0.03,
            "sampleStandardDeviation": True,
            "luminanceFloor": None,
            "zeroMeanSurfacePixelsExcluded": int(np.count_nonzero(surface_hit_mask & ~positive_mean)),
        },
        "passed": gate,
    }
    if reference_hdr is not None:
        interactive_image = load_radiance_hdr(selected[-1])
        reference_image = load_radiance_hdr(reference_hdr)
        result["edgeWidth10To90"] = edge_width_statistics(interactive_image, reference_image)
        result["passed"] = result["passed"] and result["edgeWidth10To90"]["passed"]
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="Benchmark output directory")
    parser.add_argument("--start", type=int, default=0, help="First discovered sequence frame")
    parser.add_argument("--count", type=int, default=32, help="Frames to analyze; 0 means all remaining")
    parser.add_argument("--output", type=Path, help="Output JSON (default: <input>/temporal-analysis.json)")
    parser.add_argument("--reference-hdr", type=Path, help="Matched high-SPP HDR for the 10-90 percent edge-width gate")
    parser.add_argument("--enforce", action="store_true", help="Return nonzero when the CV/finite gate fails")
    arguments = parser.parse_args()
    if arguments.start < 0 or arguments.count < 0:
        parser.error("--start and --count must be non-negative")
    input_directory = arguments.input.resolve()
    files = discover_hdr_sequence(input_directory)
    selected_files = files[
        arguments.start:arguments.start + arguments.count if arguments.count > 0 else None]
    hit_mask_selection = discover_surface_hit_masks(input_directory, selected_files)
    reference_hdr = arguments.reference_hdr.resolve() if arguments.reference_hdr else None
    result = analyze(files, arguments.start, arguments.count, reference_hdr, hit_mask_selection)
    output = arguments.output or input_directory / "temporal-analysis.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 2 if arguments.enforce and not result["passed"] else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        print(f"analysis failed: {exception}", file=sys.stderr)
        raise SystemExit(1)
