import argparse
import re
import shutil
from dataclasses import dataclass
from pathlib import Path

import cv2 as cv
import numpy as np

ROI_ORDER = [
    "x_sign",
    "x_100",
    "x_10",
    "x_1",
    "y_sign",
    "y_100",
    "y_10",
    "y_1",
]

FILENAME_RE = re.compile(
    r"^f(?P<frame>\d+)_(?P<roi>x_sign|x_100|x_10|x_1|y_sign|y_100|y_10|y_1)\.png$"
)


@dataclass
class FrameRois:
    frame: int
    files: dict[str, Path]


def split_digits(value: int):
    abs_v = abs(value)
    if abs_v > 999:
        raise ValueError(f"value out of range for 3 digits: {value}")

    return [int(ch) for ch in str(abs_v)]


def make_axis_slots(value: int):
    digits = split_digits(value)
    slots: dict[str, int | str | None] = {
        "sign": None,
        "100": None,
        "10": None,
        "1": None,
    }

    start = 4 - len(digits)
    order = ["sign", "100", "10", "1"]
    for i, d in enumerate(digits):
        slots[order[start + i]] = d

    if value < 0:
        minus_index = start - 1
        if minus_index < 0:
            raise ValueError(f"cannot place minus for value: {value}")
        slots[order[minus_index]] = "-"

    return slots


def discover_frames(raw_dir: Path):
    groups: dict[int, dict[str, Path]] = {}
    for path in sorted(raw_dir.glob("f*_*.png")):
        m = FILENAME_RE.match(path.name)
        if not m:
            continue
        frame = int(m.group("frame"))
        roi = m.group("roi")
        groups.setdefault(frame, {})[roi] = path

    frames: list[FrameRois] = []
    for frame, files in sorted(groups.items()):
        if all(k in files for k in ROI_ORDER):
            frames.append(FrameRois(frame=frame, files=files))
    return frames


def make_preview(files: dict[str, Path], tile_w=240, tile_h=80):
    rows = []
    for row_keys in (
        ("x_sign", "x_100", "x_10", "x_1"),
        ("y_sign", "y_100", "y_10", "y_1"),
    ):
        tiles = []
        for key in row_keys:
            img = cv.imread(str(files[key]))
            if img is None:
                img = np.zeros((tile_h, tile_w, 3), dtype=np.uint8)
            else:
                img = cv.resize(img, (tile_w, tile_h), interpolation=cv.INTER_NEAREST)
            cv.putText(
                img,
                key,
                (8, 22),
                cv.FONT_HERSHEY_SIMPLEX,
                0.6,
                (0, 0, 0),
                3,
                cv.LINE_AA,
            )
            cv.putText(
                img,
                key,
                (8, 22),
                cv.FONT_HERSHEY_SIMPLEX,
                0.6,
                (255, 255, 255),
                1,
                cv.LINE_AA,
            )
            tiles.append(img)
        rows.append(np.concatenate(tiles, axis=1))
    return np.concatenate(rows, axis=0)


def save_templates_for_axis(
    axis: str,
    value: int,
    frame: int,
    files: dict[str, Path],
    out_dir: Path,
    save_plus_sign: bool,
    save_blank_digits: bool,
    save_blank_signs: bool,
):
    slots = make_axis_slots(value)
    labels = {
        f"{axis}_sign": slots["sign"],
        f"{axis}_100": slots["100"],
        f"{axis}_10": slots["10"],
        f"{axis}_1": slots["1"],
    }

    saved = 0
    for roi_key, token in labels.items():
        if token is None:
            if save_blank_signs and roi_key.endswith("_sign"):
                src = files[roi_key]
                dst = out_dir / f"blank_sign_f{frame:06d}_{roi_key}.png"
                shutil.copy2(src, dst)
                saved += 1
            if save_blank_digits and roi_key.endswith(("_100", "_10")):
                src = files[roi_key]
                dst = out_dir / f"blank_digit_f{frame:06d}_{roi_key}.png"
                shutil.copy2(src, dst)
                saved += 1
            continue
        src = files[roi_key]
        if token == "-":
            dst = out_dir / f"sign_minus_f{frame:06d}_{roi_key}.png"
            shutil.copy2(src, dst)
            saved += 1
            continue

        if isinstance(token, int):
            dst = out_dir / f"digit_{token}_f{frame:06d}_{roi_key}.png"
            shutil.copy2(src, dst)
            saved += 1

    if value >= 0 and save_plus_sign:
        src = files[f"{axis}_sign"]
        dst = out_dir / f"sign_plus_f{frame:06d}_{axis}_sign.png"
        shutil.copy2(src, dst)
        saved += 1

    return saved


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--raw-dir", required=True, help="Directory created by --dump-roi-dir"
    )
    ap.add_argument("--out-dir", required=True, help="Output template directory")
    ap.add_argument("--start-frame", type=int, default=None)
    ap.add_argument("--end-frame", type=int, default=None)
    ap.add_argument(
        "--save-plus-sign",
        action="store_true",
        help="Also save sign_plus templates when value is non-negative",
    )
    ap.add_argument(
        "--no-save-blank-digits",
        action="store_true",
        help="Do not save blank_digit templates for empty 100/10 slots",
    )
    ap.add_argument(
        "--no-save-blank-signs",
        action="store_true",
        help="Do not save blank_sign templates for empty sign slots",
    )
    return ap.parse_args()


def main():
    args = parse_args()
    raw_dir = Path(args.raw_dir)
    out_dir = Path(args.out_dir)

    if not raw_dir.exists():
        raise FileNotFoundError(f"raw dir not found: {raw_dir}")

    out_dir.mkdir(parents=True, exist_ok=True)

    frames = discover_frames(raw_dir)
    if args.start_frame is not None:
        frames = [fr for fr in frames if fr.frame >= args.start_frame]
    if args.end_frame is not None:
        frames = [fr for fr in frames if fr.frame <= args.end_frame]

    if not frames:
        print("No complete frame groups found.")
        return

    win = "template_labeler"
    cv.namedWindow(win, cv.WINDOW_NORMAL)

    print("Enter gx gy (e.g. -12 34), 's' to skip, 'q' to quit")
    if not args.no_save_blank_digits:
        print("blank_digit templates: enabled")
    if not args.no_save_blank_signs:
        print("blank_sign templates: enabled")
    total_saved = 0
    processed = 0

    for fr in frames:
        preview = make_preview(fr.files)
        title = f"frame={fr.frame}"
        vis = preview.copy()
        cv.putText(
            vis,
            title,
            (10, vis.shape[0] - 12),
            cv.FONT_HERSHEY_SIMPLEX,
            0.8,
            (0, 0, 0),
            3,
            cv.LINE_AA,
        )
        cv.putText(
            vis,
            title,
            (10, vis.shape[0] - 12),
            cv.FONT_HERSHEY_SIMPLEX,
            0.8,
            (255, 255, 255),
            1,
            cv.LINE_AA,
        )
        cv.imshow(win, vis)
        cv.waitKey(1)

        line = input(f"[frame {fr.frame}] gx gy / s / q > ").strip()
        if not line:
            continue
        if line.lower() == "q":
            break
        if line.lower() == "s":
            processed += 1
            continue

        parts = line.split()
        if len(parts) != 2:
            print("Invalid input. expected: gx gy")
            continue

        try:
            gx = int(parts[0])
            gy = int(parts[1])
        except ValueError:
            print("Invalid numbers.")
            continue

        saved = 0
        saved += save_templates_for_axis(
            axis="x",
            value=gx,
            frame=fr.frame,
            files=fr.files,
            out_dir=out_dir,
            save_plus_sign=args.save_plus_sign,
            save_blank_digits=not args.no_save_blank_digits,
            save_blank_signs=not args.no_save_blank_signs,
        )
        saved += save_templates_for_axis(
            axis="y",
            value=gy,
            frame=fr.frame,
            files=fr.files,
            out_dir=out_dir,
            save_plus_sign=args.save_plus_sign,
            save_blank_digits=not args.no_save_blank_digits,
            save_blank_signs=not args.no_save_blank_signs,
        )

        total_saved += saved
        processed += 1
        print(f"saved {saved} templates from frame {fr.frame}")

    cv.destroyAllWindows()
    print(f"done. processed={processed} saved={total_saved} out={out_dir}")


if __name__ == "__main__":
    main()
