import glob
from dataclasses import dataclass
from pathlib import Path

import cv2 as cv
import numpy as np

from .roi_io import crop


@dataclass(frozen=True)
class AxisReadResult:
    value: int
    sign: int
    digits: list[int | None]
    confidence: float


@dataclass
class TemplateBank:
    digit_templates: dict[int, list[np.ndarray]]
    blank_digit_templates: list[np.ndarray]
    blank_sign_templates: list[np.ndarray]
    minus_templates: list[np.ndarray]
    minus_templates_digit: list[np.ndarray]
    plus_templates: list[np.ndarray]
    digit_size: tuple[int, int]
    sign_size: tuple[int, int]
    digit_template_masks: dict[int, np.ndarray]
    blank_digit_template_masks: np.ndarray | None
    blank_sign_template_masks: np.ndarray | None
    minus_template_masks: np.ndarray | None
    minus_template_digit_masks: np.ndarray | None
    plus_template_masks: np.ndarray | None


def _stack_template_masks(templates: list[np.ndarray]) -> np.ndarray | None:
    if not templates:
        return None
    return np.stack([(t > 0) for t in templates], axis=0)


def _max_iou_with_masks(sample_mask: np.ndarray, template_masks: np.ndarray | None) -> float:
    if template_masks is None or template_masks.size == 0:
        return -1.0

    inter = np.logical_and(template_masks, sample_mask).sum(axis=(1, 2), dtype=np.int64)
    union = np.logical_or(template_masks, sample_mask).sum(axis=(1, 2), dtype=np.int64)

    scores = np.zeros_like(inter, dtype=np.float64)
    valid = union > 0
    scores[valid] = inter[valid] / union[valid]
    return float(scores.max(initial=-1.0))


def preprocess_to_binary(
    roi_bgr,
    target_size: tuple[int, int],
    min_area=12,
    white_threshold=200,
):
    gray = cv.cvtColor(roi_bgr, cv.COLOR_BGR2GRAY)
    gray = cv.GaussianBlur(gray, (3, 3), 0)
    bw = np.where(gray >= white_threshold, 255, 0).astype(np.uint8)

    kernel = np.ones((2, 2), dtype=np.uint8)
    bw = cv.morphologyEx(bw, cv.MORPH_OPEN, kernel)

    ys, xs = np.where(bw > 0)
    h, w = bw.shape[:2]
    if len(xs) == 0:
        return np.zeros((target_size[1], target_size[0]), dtype=np.uint8), 0.0

    x0, x1 = xs.min(), xs.max()
    y0, y1 = ys.min(), ys.max()
    area = (x1 - x0 + 1) * (y1 - y0 + 1)
    if area < min_area:
        return np.zeros((target_size[1], target_size[0]), dtype=np.uint8), 0.0

    pad = 1
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(w - 1, x1 + pad)
    y1 = min(h - 1, y1 + pad)
    glyph = bw[y0 : y1 + 1, x0 : x1 + 1]

    tw, th = target_size
    gh, gw = glyph.shape[:2]
    scale = min(tw / max(1, gw), th / max(1, gh))
    nw = max(1, int(round(gw * scale)))
    nh = max(1, int(round(gh * scale)))
    resized = cv.resize(glyph, (nw, nh), interpolation=cv.INTER_NEAREST)

    canvas = np.zeros((th, tw), dtype=np.uint8)
    ox = (tw - nw) // 2
    oy = (th - nh) // 2
    canvas[oy : oy + nh, ox : ox + nw] = resized
    ink_ratio = float((canvas > 0).mean())
    return canvas, ink_ratio


def load_templates(templates_dir: str | None) -> TemplateBank | None:
    if not templates_dir:
        return None

    base = Path(templates_dir)
    if not base.exists():
        raise FileNotFoundError(f"Template dir not found: {templates_dir}")

    digit_templates: dict[int, list[np.ndarray]] = {d: [] for d in range(10)}
    blank_digit_templates: list[np.ndarray] = []
    blank_sign_templates: list[np.ndarray] = []
    minus_templates: list[np.ndarray] = []
    minus_templates_digit: list[np.ndarray] = []
    plus_templates: list[np.ndarray] = []

    digit_size = (40, 64)
    sign_size = (32, 20)

    for d in range(10):
        pattern = str(base / f"digit_{d}*.png")
        for p in sorted(glob.glob(pattern)):
            img = cv.imread(p)
            if img is None:
                continue
            proc, _ = preprocess_to_binary(img, digit_size)
            digit_templates[d].append(proc)

    for p in sorted(glob.glob(str(base / "blank_digit*.png"))):
        img = cv.imread(p)
        if img is None:
            continue
        proc, _ = preprocess_to_binary(img, digit_size)
        blank_digit_templates.append(proc)

    for p in sorted(glob.glob(str(base / "blank_sign*.png"))):
        img = cv.imread(p)
        if img is None:
            continue
        proc, _ = preprocess_to_binary(img, sign_size)
        blank_sign_templates.append(proc)

    for p in sorted(glob.glob(str(base / "sign_minus*.png"))):
        img = cv.imread(p)
        if img is None:
            continue
        proc_sign, _ = preprocess_to_binary(img, sign_size)
        proc_digit, _ = preprocess_to_binary(img, digit_size)
        minus_templates.append(proc_sign)
        minus_templates_digit.append(proc_digit)

    for p in sorted(glob.glob(str(base / "sign_plus*.png"))):
        img = cv.imread(p)
        if img is None:
            continue
        proc, _ = preprocess_to_binary(img, sign_size)
        plus_templates.append(proc)

    if not any(len(v) > 0 for v in digit_templates.values()):
        raise ValueError(f"No digit templates found in: {templates_dir}")

    digit_template_masks = {
        d: _stack_template_masks(tmpls) for d, tmpls in digit_templates.items()
    }
    blank_digit_template_masks = _stack_template_masks(blank_digit_templates)
    blank_sign_template_masks = _stack_template_masks(blank_sign_templates)
    minus_template_masks = _stack_template_masks(minus_templates)
    minus_template_digit_masks = _stack_template_masks(minus_templates_digit)
    plus_template_masks = _stack_template_masks(plus_templates)

    return TemplateBank(
        digit_templates=digit_templates,
        blank_digit_templates=blank_digit_templates,
        blank_sign_templates=blank_sign_templates,
        minus_templates=minus_templates,
        minus_templates_digit=minus_templates_digit,
        plus_templates=plus_templates,
        digit_size=digit_size,
        sign_size=sign_size,
        digit_template_masks=digit_template_masks,
        blank_digit_template_masks=blank_digit_template_masks,
        blank_sign_template_masks=blank_sign_template_masks,
        minus_template_masks=minus_template_masks,
        minus_template_digit_masks=minus_template_digit_masks,
        plus_template_masks=plus_template_masks,
    )


def compare_binary(a: np.ndarray, b: np.ndarray) -> float:
    aa = (a > 0).astype(np.float32)
    bb = (b > 0).astype(np.float32)
    inter = float((aa * bb).sum())
    union = float(np.clip(aa + bb, 0, 1).sum())
    if union <= 0:
        return 0.0
    return inter / union


def predict_digit(sample: np.ndarray, bank: TemplateBank):
    sample_mask = sample > 0
    best_digit = 0
    best_score = -1.0
    for digit, template_masks in bank.digit_template_masks.items():
        score = _max_iou_with_masks(sample_mask, template_masks)
        if score > best_score:
            best_score = score
            best_digit = digit
    return best_digit, best_score


def predict_minus_score_from_sample(
    sample: np.ndarray, template_masks: np.ndarray | None
):
    return _max_iou_with_masks(sample > 0, template_masks)


def predict_minus_score(
    roi_bgr,
    target_size: tuple[int, int],
    templates: list[np.ndarray],
    template_masks: np.ndarray | None = None,
):
    if not templates and template_masks is None:
        return -1.0, 0.0

    sample, ink_ratio = preprocess_to_binary(roi_bgr, target_size)
    if template_masks is not None:
        best = predict_minus_score_from_sample(sample, template_masks)
    else:
        best = -1.0
        for tmpl in templates:
            best = max(best, compare_binary(sample, tmpl))
    return best, ink_ratio


def classify_sign_roi(
    roi_bgr,
    bank: TemplateBank,
    *,
    minus_thresh: float = 0.35,
    blank_ink_thresh: float = 0.05,
    blank_template_margin: float = 0.03,
    preprocessed: tuple[np.ndarray, float] | None = None,
):
    if preprocessed is None:
        sample, ink_ratio = preprocess_to_binary(roi_bgr, bank.sign_size)
    else:
        sample, ink_ratio = preprocessed

    sample_mask = sample > 0
    minus_score = _max_iou_with_masks(sample_mask, bank.minus_template_masks)
    plus_score = _max_iou_with_masks(sample_mask, bank.plus_template_masks)
    blank_score = _max_iou_with_masks(sample_mask, bank.blank_sign_template_masks)

    is_blank = ink_ratio < blank_ink_thresh
    if not is_blank and blank_score >= 0:
        is_blank = (
            blank_score >= minus_score + blank_template_margin
            and blank_score >= plus_score + blank_template_margin
        )

    if is_blank:
        token = None
    elif minus_score >= minus_thresh:
        token = "-"
    elif plus_score >= minus_thresh:
        token = "+"
    else:
        token = None

    return token, minus_score, blank_score, plus_score, ink_ratio


def classify_digit_roi(
    roi_bgr,
    bank: TemplateBank,
    *,
    allow_blank: bool,
    minus_thresh: float = 0.35,
    blank_ink_thresh: float = 0.05,
    blank_digit_score_thresh: float = 0.22,
    blank_template_margin: float = 0.03,
    preprocessed: tuple[np.ndarray, float] | None = None,
    minus_score_override: float | None = None,
):
    if preprocessed is None:
        sample, ink_ratio = preprocess_to_binary(roi_bgr, bank.digit_size)
    else:
        sample, ink_ratio = preprocessed

    sample_mask = sample > 0
    digit, digit_score = predict_digit(sample, bank)
    if minus_score_override is None:
        minus_score = _max_iou_with_masks(sample_mask, bank.minus_template_digit_masks)
    else:
        minus_score = minus_score_override

    blank_score = _max_iou_with_masks(sample_mask, bank.blank_digit_template_masks)

    is_blank = False
    if allow_blank:
        is_blank = ink_ratio < blank_ink_thresh
        if not is_blank and blank_score >= 0:
            is_blank = (
                blank_score >= digit_score + blank_template_margin
                and blank_score >= minus_score + blank_template_margin
            )
        if not is_blank:
            is_blank = (
                digit_score < blank_digit_score_thresh and minus_score < minus_thresh
            )

    if is_blank:
        token = None
    elif minus_score >= minus_thresh:
        token = "-"
    else:
        token = digit

    return token, digit_score, minus_score, blank_score, ink_ratio


def decode_axis_value(frame, rois, axis: str, bank: TemplateBank):
    minus_thresh = 0.35

    sign_roi = crop(frame, rois[f"{axis}_sign"])
    sign_preprocessed = preprocess_to_binary(sign_roi, bank.sign_size)
    sign_token, minus_sign_score, _, _, ink_sign = classify_sign_roi(
        sign_roi,
        bank,
        minus_thresh=minus_thresh,
        preprocessed=sign_preprocessed,
    )

    d100_roi = crop(frame, rois[f"{axis}_100"])
    d100_preprocessed = preprocess_to_binary(d100_roi, bank.digit_size)
    minus_100_score = predict_minus_score_from_sample(
        d100_preprocessed[0], bank.minus_template_digit_masks
    )

    d10_roi = crop(frame, rois[f"{axis}_10"])
    d10_preprocessed = preprocess_to_binary(d10_roi, bank.digit_size)
    minus_10_score = predict_minus_score_from_sample(
        d10_preprocessed[0], bank.minus_template_digit_masks
    )

    d1_roi = crop(frame, rois[f"{axis}_1"])
    d1_preprocessed = preprocess_to_binary(d1_roi, bank.digit_size)

    minus_candidates = [
        ("sign", minus_sign_score if sign_token == "-" else -1.0),
        ("100", minus_100_score),
        ("10", minus_10_score),
    ]
    minus_slot = None
    minus_best = max(score for _, score in minus_candidates)
    if minus_best >= minus_thresh:
        minus_slot = max(minus_candidates, key=lambda t: t[1])[0]

    sign = -1 if minus_slot is not None else 1
    sign_score = 1.0 if minus_slot is None else minus_best

    digits: list[int | None] = []
    scores = [sign_score]
    for place in (100, 10, 1):
        if minus_slot == str(place):
            digits.append(None)
            scores.append(minus_best)
            continue

        if place == 100:
            digit_roi = d100_roi
            digit_preprocessed = d100_preprocessed
            minus_override = minus_100_score
        elif place == 10:
            digit_roi = d10_roi
            digit_preprocessed = d10_preprocessed
            minus_override = minus_10_score
        else:
            digit_roi = d1_roi
            digit_preprocessed = d1_preprocessed
            minus_override = None

        token, digit_score, minus_digit_score, _, _ = classify_digit_roi(
            digit_roi,
            bank,
            allow_blank=(place != 1),
            minus_thresh=minus_thresh,
            preprocessed=digit_preprocessed,
            minus_score_override=minus_override,
        )

        if token is None:
            digits.append(None)
            scores.append(1.0)
            continue

        if token == "-":
            digits.append(None)
            scores.append(minus_digit_score)
            continue

        digits.append(int(token))
        scores.append(digit_score)

    d100, d10, d1 = digits

    if sign < 0:
        if minus_slot == "sign":
            if d100 is None or d10 is None or d1 is None:
                sign_score = min(sign_score, 0.0)
        elif minus_slot == "100":
            if d100 is not None or d10 is None or d1 is None or ink_sign >= 0.02:
                sign_score = min(sign_score, 0.0)
        elif minus_slot == "10":
            if d100 is not None or d10 is not None or d1 is None or ink_sign >= 0.02:
                sign_score = min(sign_score, 0.0)

    value_abs = (
        (0 if d100 is None else d100 * 100)
        + (0 if d10 is None else d10 * 10)
        + (0 if d1 is None else d1)
    )
    if d100 is None and d10 is None and d1 is None:
        value_abs = 0

    return AxisReadResult(
        value=sign * value_abs,
        sign=sign,
        digits=digits,
        confidence=float(min(scores)),
    )
