#!/usr/bin/env python3
"""Small headless Vulkan AO regression: python tests/ambient_occlusion_gpu.py.

Requires glslc, spirv-val and the Python 'vulkan' package (pip install vulkan).
No game files, window or surface are used.
"""
import math
import unittest

W, H = 63, 47
A, B = 1.002002002, 2.002002002


def depth(distance):
    return (-A + B / distance + 1.0) * 0.5


def plane(w, h, slope=0.0, offset_x=0.0):
    return [depth(10.0 / (1.0 + slope * (2.0 * (x + 0.5) / w - 1.0 + offset_x)))
            for y in range(h) for x in range(w)]


class AmbientOcclusion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from ambient_occlusion_vulkan import Runner
        cls.runner = Runner()
        cls.addClassCleanup(cls.runner.close)

    def render(self, values, mode, strength=1.0, radius=4.0, w=W, h=H, dw=W, dh=H):
        return self.runner.render(values, w, h, dw, dh, mode, strength, radius)

    def test_synthetic_depths(self):
        for mode in (1, 2):
            with self.subTest(mode=mode):
                flat = plane(W, H)
                self.assertTrue(all(c == 1.0 for c in self.render(flat, mode, strength=0)))
                self.assertTrue(all(c == 1.0 for c in self.render([0.0] * (W * H), mode)))
                for slope in (0.0, 0.35, -0.35):
                    result = self.render(plane(W, H, slope), mode)
                    self.assertGreater(min(result), 0.97, f'plane slope {slope}')
                corner = flat[:]
                for y in range(H):
                    for x in range(W // 2, W):
                        corner[y * W + x] = depth(8.0)
                result = self.render(corner, mode)
                self.assertLess(min(result[y * W + x] for y in range(H) for x in range(W // 2)), 0.98, 'near occluder must darken the background')
                for quality in (0, 1):
                    result = self.runner.render(corner, W, H, W, H, mode, 1.0, 4.0, quality)
                    self.assertLess(min(result[y * W + x] for y in range(H) for x in range(W // 2)), .99, f'quality {quality} must occlude')
                far = flat[:]
                for y in range(H):
                    for x in range(W // 2, W):
                        far[y * W + x] = depth(2.0)
                self.assertGreater(min(self.render(far, mode)), 0.97, 'far depth jump must not halo')
                invalid = flat[:]
                invalid[10] = float('nan')
                invalid[11] = float('inf')
                self.assertTrue(all(math.isfinite(c) for c in self.render(invalid, mode)))
                dw, dh = 31, 23
                self.assertGreater(min(self.render(plane(dw, dh, .35), mode, dw=dw, dh=dh)), .97)
                offset = .23
                result = self.runner.render(plane(W, H, .35, offset), W, H, W, H, mode, 1, 4, 2, offset)
                self.assertGreater(min(result), .97, 'offset projection plane')


if __name__ == '__main__':
    unittest.main(verbosity=2)
