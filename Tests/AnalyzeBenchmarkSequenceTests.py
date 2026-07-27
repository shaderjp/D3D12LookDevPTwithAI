#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np


REPOSITORY = Path(__file__).resolve().parents[1]
MODULE_PATH = REPOSITORY / "Scripts" / "AnalyzeBenchmarkSequence.py"
SPEC = importlib.util.spec_from_file_location("analyze_benchmark_sequence", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not import {MODULE_PATH}")
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def write_rgba16_dds(
        path: Path, image: np.ndarray, *, dx10: bool = False, row_padding: int = 0) -> None:
    image = np.asarray(image, dtype="<f2")
    height, width, channels = image.shape
    if channels != 4:
        raise ValueError("Test DDS must have four channels")
    tight_pitch = width * 8
    row_pitch = tight_pitch + row_padding
    header = bytearray(124)
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    struct.pack_into("<7I", header, 0, 124, flags, height, width, row_pitch, 0, 1)
    four_cc = int.from_bytes(b"DX10", "little") if dx10 else 113
    struct.pack_into("<8I", header, 72, 32, 0x4, four_cc, 0, 0, 0, 0, 0)
    struct.pack_into("<I", header, 104, 0x1000)
    extension = struct.pack("<5I", 10, 3, 0, 1, 0) if dx10 else b""
    rows = []
    for row in image:
        rows.append(row.tobytes() + bytes(row_padding))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"DDS " + header + extension + b"".join(rows))


class DdsReaderTests(unittest.TestCase):
    def test_loads_legacy_directxtex_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "legacy.dds"
            expected = np.array(
                [[[1.0, -2.0, 3.5, 1.0], [4.0, 5.0, 6.0, 0.0]]], dtype=np.float16)
            write_rgba16_dds(path, expected)
            np.testing.assert_array_equal(ANALYZER.load_dds_rgba16_float(path), expected)

    def test_loads_dx10_header_and_padded_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "dx10.dds"
            expected = np.arange(24, dtype=np.float16).reshape(2, 3, 4)
            write_rgba16_dds(path, expected, dx10=True, row_padding=16)
            np.testing.assert_array_equal(ANALYZER.load_dds_rgba16_float(path), expected)


class SurfaceMaskTests(unittest.TestCase):
    def test_prefers_complete_per_frame_surface_guides(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts = []
            hdrs = []
            for measured in range(2):
                frame = root / "frames" / f"{measured:06d}"
                hdr = frame / "beauty_hdr.hdr"
                hdr.parent.mkdir(parents=True, exist_ok=True)
                hdr.write_bytes(b"test")
                mask = frame / "surface_motion_hit.dds"
                write_rgba16_dds(mask, np.zeros((1, 1, 4), dtype=np.float16))
                hdrs.append(hdr.resolve())
                artifacts.extend((
                    {"path": hdr.relative_to(root).as_posix(), "role": "beauty_hdr",
                     "phase": "measured", "measuredFrameIndex": measured},
                    {"path": mask.relative_to(root).as_posix(), "role": "surface_motion_hit",
                     "phase": "measured", "measuredFrameIndex": measured,
                     "sourceFormat": "R16G16B16A16_FLOAT"},
                ))
            final_mask = root / "surface_motion_hit.dds"
            write_rgba16_dds(final_mask, np.zeros((1, 1, 4), dtype=np.float16))
            artifacts.append({"path": final_mask.name, "role": "surface_motion_hit",
                              "phase": "final", "sourceFormat": "R16G16B16A16_FLOAT"})
            (root / "artifacts.json").write_text(
                json.dumps({"artifacts": artifacts}), encoding="utf-8")

            selection = ANALYZER.discover_surface_hit_masks(root, hdrs)
            self.assertEqual(selection.source, "perFrameSurfaceGuidesArtifacts")
            self.assertEqual(selection.per_frame_available, 2)
            self.assertEqual(selection.paths[0].parent.name, "000000")
            self.assertEqual(selection.paths[1].parent.name, "000001")

    def test_cv_uses_hit_mask_and_no_luminance_floor(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mask_path = root / "surface_motion_hit.dds"
            # Pixel 0 is a very dark surface. Pixel 1 is sky and must not enter
            # the surface CV even though its radiance changes dramatically.
            mask = np.zeros((1, 2, 4), dtype=np.float16)
            # primaryHitT is not a boolean. A nearby surface can legitimately
            # have a value below 0.5 and must still be included.
            mask[0, 0, 3] = 0.25
            write_rgba16_dds(mask_path, mask)
            frame0 = root / "frame0.hdr"
            frame1 = root / "frame1.hdr"
            dark = np.float32(1.0e-8)
            images = {
                frame0: np.array([[[dark, dark, dark], [1.0, 1.0, 1.0]]], dtype=np.float32),
                frame1: np.array([[[dark, dark, dark], [100.0, 100.0, 100.0]]], dtype=np.float32),
            }
            selection = ANALYZER.SurfaceHitMaskSelection(
                paths=(mask_path,),
                source="finalStaticCameraSurfaceGuidesArtifact",
                combination="singleFinalStaticCameraMask",
                per_frame_available=0)
            with mock.patch.object(ANALYZER, "load_radiance_hdr", side_effect=lambda path: images[path]):
                result = ANALYZER.analyze([frame0, frame1], 0, 2, None, selection)

            self.assertEqual(result["validLuminancePixels"], 1)
            self.assertEqual(result["surfaceHitMask"]["intersectionSurfacePixels"], 1)
            self.assertEqual(result["surfaceHitMask"]["hitThresholdExclusive"], 0.0)
            self.assertEqual(result["surfaceLuminanceTemporalCv"]["median"], 0.0)
            self.assertIsNone(result["surfaceLuminanceTemporalCv"]["luminanceFloor"])
            self.assertTrue(result["passed"])


if __name__ == "__main__":
    unittest.main()
