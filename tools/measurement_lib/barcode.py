from dataclasses import dataclass

import cv2 as cv
import numpy as np

from .roi_io import Roi


@dataclass(frozen=True)
class BarcodeDecoded:
    frame: int
    sx: int
    sy: int
    crc_in: int
    crc_calc: int
    preamble_ok: bool
    crc_ok: bool


def barcode_windows(width: int, n_bits: int, left=0.45, right=0.55):
    wins = []
    for i in range(n_bits):
        x0 = int((i + left) * width / n_bits)
        x1 = int((i + right) * width / n_bits)

        x0 = max(0, min(width - 1, x0))
        x1 = max(0, min(width, x1))
        if x1 <= x0:
            x1 = min(width, x0 + 1)

        wins.append((x0, x1))
    return wins


def decode_barcode(barcode_bgr, n_bits: int):
    gray = cv.cvtColor(barcode_bgr, cv.COLOR_BGR2GRAY)
    signal = gray.mean(axis=0)
    threshold = (float(signal.min()) + float(signal.max())) / 2.0

    width = signal.shape[0]
    wins = barcode_windows(width, n_bits)
    starts = np.array([x0 for x0, _ in wins], dtype=np.int32)
    ends = np.array([x1 for _, x1 in wins], dtype=np.int32)
    lengths = np.maximum(ends - starts, 1)

    csum = np.concatenate(([0.0], np.cumsum(signal, dtype=np.float64)))
    sums = csum[ends] - csum[starts]
    means = sums / lengths
    bits_arr = (means >= threshold).astype(np.uint8)
    bits = bits_arr.tolist()

    value = 0
    for bit in bits:
        value = (value << 1) | bit
    return value, bits, wins


def crc8_atm(data: bytes, poly: int = 0x07, init: int = 0x00) -> int:
    crc = init & 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) & 0xFF) ^ poly
            else:
                crc = (crc << 1) & 0xFF
    return crc & 0xFF


def parse_barcode_bits(bits: list[int]) -> BarcodeDecoded | None:
    if len(bits) != 48:
        return None

    vals = []
    for i in range(6):
        b = 0
        for bit in bits[i * 8 : (i + 1) * 8]:
            b = (b << 1) | bit
        vals.append(b)

    preamble = vals[0]
    frame = ((vals[1] & 0xFF) << 8) | (vals[2] & 0xFF)
    sx = vals[3] & 0xFF
    sy = vals[4] & 0xFF
    crc_in = vals[5] & 0xFF
    payload = bytes([vals[1], vals[2], vals[3], vals[4]])
    crc_calc = crc8_atm(payload)

    return BarcodeDecoded(
        frame=frame,
        sx=sx,
        sy=sy,
        crc_in=crc_in,
        crc_calc=crc_calc,
        preamble_ok=(preamble == 0xA5),
        crc_ok=(crc_in == crc_calc),
    )


def draw_barcode_windows(img, roi: Roi, wins, thickness=1):
    vis = img.copy()
    y0, y1 = roi.y, roi.y + roi.h - 1
    for x0, x1 in wins:
        fx0 = roi.x + x0
        fx1 = roi.x + x1 - 1
        cv.rectangle(vis, (fx0, y0), (fx1, y1), (0, 255, 0), thickness)
        cx = (fx0 + fx1) // 2
        cy = (y0 + y1) // 2
        cv.circle(vis, (cx, cy), 2, (0, 0, 255), -1)
    return vis
