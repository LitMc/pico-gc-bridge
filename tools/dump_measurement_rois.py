import argparse
from pathlib import Path

import cv2 as cv
from measurement_lib import Roi, crop, ensure_roi_names, load_rois

GAME_ROI_NAMES = [
    "x_sign",
    "x_100",
    "x_10",
    "x_1",
    "y_sign",
    "y_100",
    "y_10",
    "y_1",
]


def read_frame_at(cap: cv.VideoCapture, idx: int):
    cap.set(cv.CAP_PROP_POS_FRAMES, idx)
    ret, frame = cap.read()
    return ret, frame


def draw_overlay(img, text, x=10, y=30, size=0.7):
    vis = img.copy()
    cv.putText(
        vis, text, (x, y), cv.FONT_HERSHEY_SIMPLEX, size, (0, 0, 0), 4, cv.LINE_AA
    )
    cv.putText(
        vis,
        text,
        (x, y),
        cv.FONT_HERSHEY_SIMPLEX,
        size,
        (255, 255, 255),
        2,
        cv.LINE_AA,
    )
    return vis


def draw_rectangle(img, roi: Roi, color=(0, 255, 0), thickness=1):
    vis = img.copy()
    cv.rectangle(
        vis,
        (roi.x, roi.y),
        (roi.x + roi.w, roi.y + roi.h),
        color,
        thickness,
    )
    return vis


def dump_game_rois(frame, rois: dict[str, Roi], out_dir: str, frame_index: int):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name in GAME_ROI_NAMES:
        img = crop(frame, rois[name])
        p = out / f"f{frame_index:06d}_{name}.png"
        cv.imwrite(str(p), img)


def parse_frame_list(frames_arg: str | None, frames_file: str | None) -> list[int]:
    values: list[int] = []

    if frames_file:
        for line in Path(frames_file).read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            values.append(int(s))

    if frames_arg:
        for token in frames_arg.split(","):
            s = token.strip()
            if not s:
                continue
            values.append(int(s))

    return values


def run_batch_dump(
    cap: cv.VideoCapture,
    rois: dict[str, Roi],
    out_dir: str,
    frame_list: list[int],
    frame_count: int,
):
    if not frame_list:
        return

    dumped = 0
    seen: set[int] = set()
    for raw_idx in frame_list:
        idx = raw_idx
        if frame_count > 0:
            idx = max(0, min(frame_count - 1, idx))
        else:
            idx = max(0, idx)

        if idx in seen:
            continue
        seen.add(idx)

        ret, frame = read_frame_at(cap, idx)
        if not ret:
            print(f"skip frame={idx}: read failed")
            continue

        dump_game_rois(frame, rois, out_dir, idx)
        dumped += 1
        print(f"dumped frame={idx}")

    print(f"batch dump completed: {dumped} frames -> {out_dir}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True, help="Path to the video file")
    ap.add_argument("--rois", required=True, help="Path to the ROIs file")
    ap.add_argument("--out-dir", required=True, help="ROI dump output directory")
    ap.add_argument(
        "--frames",
        default=None,
        help="Comma-separated video frame indices to dump (e.g. 342,812,1222)",
    )
    ap.add_argument(
        "--frames-file",
        default=None,
        help="Text file containing one video frame index per line",
    )
    ap.add_argument(
        "--start-frame",
        type=int,
        default=0,
        help="Frame number to start processing from in manual mode",
    )
    args = ap.parse_args()

    cap = cv.VideoCapture(args.video)
    if not cap.isOpened():
        raise RuntimeError(f"Error opening video: {args.video}")

    rois = load_rois(args.rois)
    ensure_roi_names(rois, GAME_ROI_NAMES)

    cur = args.start_frame
    ret, frame = read_frame_at(cap, cur)
    if not ret:
        raise RuntimeError(f"Failed to read frame at index {cur}")

    fps = cap.get(cv.CAP_PROP_FPS)
    if fps <= 0:
        fps = 30.0
    frame_count = int(cap.get(cv.CAP_PROP_FRAME_COUNT))
    if frame_count <= 0:
        frame_count = -1

    frame_list = parse_frame_list(args.frames, args.frames_file)
    if frame_list:
        run_batch_dump(cap, rois, args.out_dir, frame_list, frame_count)
        cap.release()
        return

    paused = True
    win = "dump_measurement_rois"
    cv.namedWindow(win, cv.WINDOW_NORMAL)

    while True:
        vis = draw_overlay(
            frame,
            "q:quit  space:play/pause  ./:next  ,:prev  e:+10  f:-10  g:jump  d:dump",
            x=8,
            y=24,
            size=0.5,
        )
        vis = draw_overlay(vis, f"frame={cur}", x=8, y=44, size=0.55)
        for name in GAME_ROI_NAMES:
            vis = draw_rectangle(vis, rois[name], color=(255, 255, 0), thickness=1)

        cv.imshow(win, vis)
        delay = 0 if paused else max(1, int(1000 / fps))
        key_raw = cv.waitKey(delay)

        if key_raw == -1 and not paused:
            cur += 1
            if frame_count > 0 and cur >= frame_count:
                paused = True
                cur = frame_count - 1
                continue
            ret, frame = cap.read()
            if not ret:
                paused = True
            continue

        key = key_raw & 0xFF

        if key in (ord("q"), 27):
            break
        if key == ord(" "):
            paused = not paused
            continue
        if key in (ord("."), ord(">")):
            cur += 1
        elif key in (ord(","), ord("<")):
            cur -= 1
        elif key == ord("e"):
            cur += 10
        elif key == ord("f"):
            cur -= 10
        elif key == ord("g"):
            try:
                s = input("jump to video frame index> ").strip()
                if s:
                    cur = int(s)
            except ValueError:
                print("invalid frame index")
            paused = True
            if frame_count > 0:
                cur = max(0, min(frame_count - 1, cur))
            else:
                cur = max(0, cur)

            ret, jumped_frame = read_frame_at(cap, cur)
            if ret:
                frame = jumped_frame
            else:
                print(f"failed to jump frame: {cur}")
            continue
        elif key == ord("d"):
            dump_game_rois(frame, rois, args.out_dir, cur)
            print(f"Dumped ROIs for frame={cur} to {args.out_dir}")
            continue
        else:
            continue

        if frame_count > 0:
            cur = max(0, min(frame_count - 1, cur))
        else:
            cur = max(0, cur)
        ret, new_frame = read_frame_at(cap, cur)
        if ret:
            frame = new_frame
        else:
            paused = True

    cap.release()
    cv.destroyAllWindows()


if __name__ == "__main__":
    main()
