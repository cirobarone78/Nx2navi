"""Headless stereo shader regressions; no game or game data required."""
import math
import struct
import unittest

from stereo_vulkan import Runner

PROJ = (1.002002002, 2.002002002, -1.0, 0.0)


def depth_for(distance):
    ndc = (PROJ[0] * -distance + PROJ[1]) / (PROJ[2] * -distance + PROJ[3])
    return (ndc + 1.0) / 2.0


def colors(w, h):
    return [(x / 128, y / 128, (x + 2 * y) / 256, .75) for y in range(h) for x in range(w)]


def reference(x, y, w, h, crop, output, depth, sep, conv, has_depth):
    ow, oh, mode, swap = output
    u, vv = (x + .5) / ow, (y + .5) / oh
    if mode == 1:
        split = ow // 2
        left = x < split
        u = ((x if left else x - split) + .5) / (split if left else ow - split)
        signs = [1 if left else -1]
    elif mode == 2:
        split = oh // 2
        left = y < split
        vv = ((y if left else y - split) + .5) / (split if left else oh - split)
        signs = [1 if left else -1]
    else:
        signs = [1, -1]
    cx, cy, cw, ch = crop
    def fetch(sign):
        sign = -sign if swap else sign
        sample_u = u
        for _ in range(4):
            dx = cx + min(cw - 1, max(0, int(sample_u * cw)))
            dy = cy + min(ch - 1, max(0, int(vv * ch)))
            z = depth[dy * w + dx]
            shift = 0.0
            if has_depth and math.isfinite(z) and 0 < z < 1:
                ndc = 2 * z - 1
                den = PROJ[2] * ndc - PROJ[0]
                distance = -(PROJ[1] - PROJ[3] * ndc) / den
                if math.isfinite(distance) and distance > 0:
                    shift = max(-.03, min(.03, sep * (conv / distance - 1)))
            sample_u = max(0, min(1, u - sign * shift))
        sx = cx + min(cw - 1, max(0, int(sample_u * cw)))
        sy = cy + min(ch - 1, max(0, int(vv * ch)))
        return tuple(struct.unpack('<e', struct.pack('<e', c))[0] for c in colors(w, h)[sy * w + sx])
    a = fetch(signs[0])
    if mode != 3:
        return a
    b = fetch(signs[1])
    return a[0], b[1], b[2], a[3]


class StereoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = Runner()

    @classmethod
    def tearDownClass(cls):
        cls.runner.close()

    def render(self, w, h, output, depths, crop=None, sep=.021, conv=10, has_depth=1):
        crop = crop or (0, 0, w, h)
        pixels = self.runner.render(colors(w, h), depths, w, h, crop, output, PROJ, (sep, conv, has_depth, 0))
        self.assertEqual(len(pixels), output[0] * output[1])
        self.assertTrue(all(math.isfinite(c) for p in pixels for c in p))
        return pixels

    def compare(self, pixels, w, h, crop, output, depths, sep, conv=10, has_depth=1, points=None):
        ow, oh = output[:2]
        points = points or [(x, y) for y in range(oh) for x in range(ow)]
        for x, y in points:
            expected = reference(x, y, w, h, crop, output, depths, sep, conv, has_depth)
            with self.subTest(x=x, y=y, output=output):
                for actual, wanted in zip(pixels[y * ow + x], expected):
                    self.assertAlmostEqual(actual, wanted, delta=.0006)

    def test_odd_splits_crop_and_flat_modes(self):
        w, h = 61, 37
        crop = (5, 3, 47, 29)
        depths = [depth_for(5)] * (w * h)
        for mode in (1, 2, 3):
            output = (35, 27, mode, 0)
            for sep, has in ((0, 1), (.025, 0)):
                with self.subTest(mode=mode, sep=sep, has_depth=has):
                    pixels = self.render(w, h, output, depths, crop, sep, has_depth=has)
                    self.compare(pixels, w, h, crop, output, depths, sep, has_depth=has)

    def test_depth_parallax_and_swap(self):
        w, h = 80, 32
        crop = (0, 0, w, h)
        for distance in (5, 10, 20):
            depths = [depth_for(distance)] * (w * h)
            for mode in (1, 2, 3):
                output = (78, 30, mode, 0)
                normal = self.render(w, h, output, depths, crop)
                swapped = self.render(w, h, (78, 30, mode, 1), depths, crop)
                # Avoid nearest-texel boundaries, where float32 rounding is ambiguous.
                points = [(x, y) for y in (6, 11, 20, 25) for x in (12, 18, 27, 32, 48, 57, 67)]
                self.compare(normal, w, h, crop, output, depths, .021, points=points)
                self.compare(swapped, w, h, crop, (78, 30, mode, 1), depths, .021, points=points)
                if mode == 3 and distance != 10:
                    x, y = 27, 12
                    self.assertNotEqual(normal[y * 78 + x][2], swapped[y * 78 + x][2])
                    if distance < 10:
                        self.assertLess(normal[y * 78 + x][0], swapped[y * 78 + x][0])
                    else:
                        self.assertGreater(normal[y * 78 + x][0], swapped[y * 78 + x][0])

    def test_invalid_and_sky_depth_stays_flat(self):
        w, h = 64, 24
        for value in (float('nan'), float('inf'), -float('inf'), 0.0, 1.0):
            depths = [value] * (w * h)
            output = (64, 24, 3, 0)
            pixels = self.render(w, h, output, depths)
            self.compare(pixels, w, h, (0, 0, w, h), output, depths, .021)

    def test_depth_sampling_respects_crop_offset(self):
        w, h = 80, 32
        crop = (30, 4, 40, 24)
        depths = [depth_for(20)] * (w * h)
        for y in range(crop[1], crop[1] + crop[3]):
            for x in range(crop[0], crop[0] + crop[2]):
                depths[y * w + x] = depth_for(5)
        output = (64, 24, 3, 0)
        pixels = self.render(w, h, output, depths, crop)
        self.compare(pixels, w, h, crop, output, depths, .021, points=[(x, 10) for x in (12, 18, 24, 28, 32)])

    def test_srgb_source_matches_native_vulkan_blit(self):
        w, h = 32, 8
        source = colors(w, h)
        pixels = self.runner.render(source, [0.0] * (w * h), w, h, (0, 0, w, h),
                                    (w, h, 3, 0), PROJ, (0, 10, 0, 1), source_srgb=True)
        baseline = self.runner.render(source, [0.0] * (w * h), w, h, (0, 0, w, h),
                                      (w, h, 3, 0), PROJ, (0, 10, 0, 0), source_srgb=True, native_blit=True)
        for actual, original in zip(pixels, baseline):
            for a, b in zip(actual, original):
                self.assertAlmostEqual(a, b, delta=.001)

    def test_lower_resolution_depth(self):
        w, h = 80, 32
        depths = [depth_for(5 if x < 20 else 20) for y in range(16) for x in range(40)]
        full = [depths[(y // 2) * 40 + x // 2] for y in range(h) for x in range(w)]
        crop, output = (0, 0, w, h), (78, 30, 3, 0)
        pixels = self.runner.render(colors(w, h), depths, w, h, crop, output,
                                    PROJ, (.021, 10, 1, 0), depth_shape=(40, 16))
        self.compare(pixels, w, h, crop, output, full, .021,
                     points=[(x, 10) for x in (6, 12, 22, 28, 52, 62, 70)])


if __name__ == '__main__':
    unittest.main()
