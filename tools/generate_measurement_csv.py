import argparse
import concurrent.futures
import csv
import time
from dataclasses import dataclass
from pathlib import Path

import cv2 as cv
from measurement_lib import (
    ObservationRow,
    aggregate_longest_run,
    crop,
    decode_axis_value,
    decode_barcode,
    ensure_roi_names,
    load_rois,
    load_templates,
    parse_barcode_bits,
)

REQUIRED_ROIS = [
    "barcode",
    "x_sign",
    "x_100",
    "x_10",
    "x_1",
    "y_sign",
    "y_100",
    "y_10",
    "y_1",
]


@dataclass(frozen=True)
class DiagnosticRow:
    frame: int
    support_len: int
    confidence_sum: float


@dataclass
class ScanStats:
    processed_frames: int = 0
    accepted_count: int = 0
    decode_fail_count: int = 0
    preamble_fail_count: int = 0
    crc_fail_count: int = 0
    barcode_time_s: float = 0.0
    ocr_time_s: float = 0.0
    early_eof: bool = False
    seek_prestart_skipped_frames: int = 0

    def merge(self, other: "ScanStats"):
        self.processed_frames += other.processed_frames
        self.accepted_count += other.accepted_count
        self.decode_fail_count += other.decode_fail_count
        self.preamble_fail_count += other.preamble_fail_count
        self.crc_fail_count += other.crc_fail_count
        self.barcode_time_s += other.barcode_time_s
        self.ocr_time_s += other.ocr_time_s
        self.early_eof = self.early_eof or other.early_eof
        self.seek_prestart_skipped_frames += other.seek_prestart_skipped_frames


def build_chunks(
    start_frame: int, end_frame: int, chunk_size: int
) -> list[tuple[int, int]]:
    chunks: list[tuple[int, int]] = []
    cur = start_frame
    while cur <= end_frame:
        chunk_end = min(end_frame, cur + chunk_size - 1)
        chunks.append((cur, chunk_end))
        cur = chunk_end + 1
    return chunks


def scan_chunk(
    video_path: str,
    rois_path: str,
    templates_dir: str,
    seek_frame: int,
    start_frame: int,
    end_frame: int | None,
    log_every: int = 0,
) -> tuple[list[ObservationRow], ScanStats]:
    cv.setNumThreads(1)
    cap = cv.VideoCapture(video_path)
    if not cap.isOpened():
        raise RuntimeError(f"Error opening video: {video_path}")

    rois = load_rois(rois_path)
    ensure_roi_names(rois, REQUIRED_ROIS)

    bank = load_templates(templates_dir)
    if bank is None:
        raise RuntimeError("templates are required for CSV generation")

    cap.set(cv.CAP_PROP_POS_FRAMES, seek_frame)
    video_frame_idx = seek_frame
    n_bits = 48
    observations: list[ObservationRow] = []
    stats = ScanStats()
    last_success: tuple[int, int, int, int, int, float] | None = None
    hit_eof = False

    frames_to_skip = max(0, start_frame - video_frame_idx)
    while frames_to_skip > 0:
        ret, _ = cap.read()
        if not ret:
            hit_eof = True
            break
        video_frame_idx += 1
        frames_to_skip -= 1
        stats.seek_prestart_skipped_frames += 1

    while True:
        if end_frame is not None and video_frame_idx > end_frame:
            break

        ret, frame = cap.read()
        if not ret:
            hit_eof = True
            break

        stats.processed_frames += 1

        barcode_roi = crop(frame, rois["barcode"])
        barcode_t0 = time.perf_counter()
        _, bits, _ = decode_barcode(barcode_roi, n_bits)
        barcode_dec = parse_barcode_bits(bits)
        stats.barcode_time_s += time.perf_counter() - barcode_t0
        if barcode_dec is None:
            stats.decode_fail_count += 1
        elif not barcode_dec.preamble_ok:
            stats.preamble_fail_count += 1
        elif not barcode_dec.crc_ok:
            stats.crc_fail_count += 1
        else:
            ocr_t0 = time.perf_counter()
            x_res = decode_axis_value(frame, rois, "x", bank)
            y_res = decode_axis_value(frame, rois, "y", bank)
            stats.ocr_time_s += time.perf_counter() - ocr_t0
            conf_min = min(x_res.confidence, y_res.confidence)

            observations.append(
                ObservationRow(
                    video_frame_idx=video_frame_idx,
                    raw_frame_id=barcode_dec.frame,
                    sx=barcode_dec.sx,
                    sy=barcode_dec.sy,
                    gx=x_res.value,
                    gy=y_res.value,
                    conf_min=conf_min,
                )
            )

            stats.accepted_count += 1
            last_success = (
                barcode_dec.frame,
                barcode_dec.sx,
                barcode_dec.sy,
                x_res.value,
                y_res.value,
                conf_min,
            )

        if log_every > 0 and stats.processed_frames % log_every == 0:
            sample_text = "sample=none"
            if last_success is not None:
                rf, sx, sy, gx, gy, conf = last_success
                sample_text = (
                    f"sample(raw={rf},sx={sx},sy={sy},gx={gx},gy={gy},conf={conf:.3f})"
                )
            print(
                "progress "
                f"video_frame={video_frame_idx} "
                f"processed={stats.processed_frames} "
                f"accepted={stats.accepted_count} "
                f"decode_fail={stats.decode_fail_count} "
                f"preamble_fail={stats.preamble_fail_count} "
                f"crc_fail={stats.crc_fail_count} "
                f"{sample_text}"
            )

        video_frame_idx += 1

    cap.release()

    if end_frame is not None and hit_eof and video_frame_idx <= end_frame:
        stats.early_eof = True

    return observations, stats


def resolve_end_frame(
    cap: cv.VideoCapture,
    user_end_frame: int,
) -> tuple[int | None, int]:
    frame_count = int(cap.get(cv.CAP_PROP_FRAME_COUNT))
    if frame_count <= 0:
        frame_count = -1

    if user_end_frame >= 0:
        return user_end_frame, frame_count
    if frame_count > 0:
        return frame_count - 1, frame_count
    return None, frame_count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True, help="Path to the video file")
    ap.add_argument("--rois", required=True, help="Path to the ROIs file")
    ap.add_argument(
        "--templates-dir",
        required=True,
        help="Template dir containing digit_0*.png..digit_9*.png and sign_minus*.png",
    )
    ap.add_argument("--out-csv", required=True, help="Output CSV path")
    ap.add_argument(
        "--out-diagnostics-csv",
        default=None,
        help="Optional diagnostics CSV path with support_len/confidence_sum",
    )
    ap.add_argument(
        "--start-frame", type=int, default=0, help="Video frame index to start from"
    )
    ap.add_argument(
        "--end-frame",
        type=int,
        default=-1,
        help="Video frame index to stop at (inclusive), -1 means full video",
    )
    ap.add_argument(
        "--min-support",
        type=int,
        default=2,
        help="Minimum run length to accept a raw-frame record",
    )
    ap.add_argument(
        "--log-every",
        type=int,
        default=1000,
        help="Print progress every N processed video frames (0 disables)",
    )
    ap.add_argument(
        "--profile-times",
        action="store_true",
        help="Print timing breakdown (barcode/ocr/aggregate) at the end",
    )
    ap.add_argument(
        "--workers",
        type=int,
        default=4,
        help="Number of worker processes (1 keeps single-process behavior)",
    )
    ap.add_argument(
        "--chunk-size",
        type=int,
        default=1000,
        help="Frames per task chunk in parallel mode",
    )
    args = ap.parse_args()

    if args.workers < 1:
        raise ValueError("--workers must be >= 1")
    if args.chunk_size < 1:
        raise ValueError("--chunk-size must be >= 1")

    cap = cv.VideoCapture(args.video)
    if not cap.isOpened():
        raise RuntimeError(f"Error opening video: {args.video}")

    start_frame = max(0, args.start_frame)
    end_frame, frame_count = resolve_end_frame(cap, args.end_frame)
    cap.release()

    if end_frame is not None and end_frame < start_frame:
        raise ValueError(
            f"invalid frame range: start={start_frame}, end={end_frame} (inclusive)"
        )

    observations: list[ObservationRow] = []
    stats = ScanStats()
    scan_started_at = time.perf_counter()

    use_parallel = args.workers > 1 and end_frame is not None
    chunk_count = 1

    if use_parallel:
        chunks = build_chunks(start_frame, end_frame, args.chunk_size)
        chunk_count = len(chunks)
        print(
            "parallel scan "
            f"workers={args.workers} "
            f"chunk_size={args.chunk_size} "
            f"chunks={chunk_count}"
        )

        completed_chunks = 0
        next_progress_threshold = max(1, args.log_every)
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            futures = [
                pool.submit(
                    scan_chunk,
                    args.video,
                    args.rois,
                    args.templates_dir,
                    start_frame,
                    chunk_start,
                    chunk_end,
                    0,
                )
                for chunk_start, chunk_end in chunks
            ]
            for fut in concurrent.futures.as_completed(futures):
                chunk_observations, chunk_stats = fut.result()
                observations.extend(chunk_observations)
                stats.merge(chunk_stats)
                completed_chunks += 1
                if args.log_every > 0:
                    while stats.processed_frames >= next_progress_threshold:
                        print(
                            "progress "
                            f"chunks={completed_chunks}/{chunk_count} "
                            f"processed={stats.processed_frames} "
                            f"accepted={stats.accepted_count} "
                            f"decode_fail={stats.decode_fail_count} "
                            f"preamble_fail={stats.preamble_fail_count} "
                            f"crc_fail={stats.crc_fail_count}"
                        )
                        next_progress_threshold += args.log_every
    else:
        if args.workers > 1 and end_frame is None:
            print(
                "warning: frame count is unknown, falling back to single worker for end-frame=-1"
            )

        chunk_observations, chunk_stats = scan_chunk(
            args.video,
            args.rois,
            args.templates_dir,
            start_frame,
            start_frame,
            end_frame,
            args.log_every,
        )
        observations.extend(chunk_observations)
        stats.merge(chunk_stats)

    observations.sort(
        key=lambda r: (
            r.video_frame_idx,
            r.raw_frame_id,
            r.sx,
            r.sy,
            r.gx,
            r.gy,
            r.conf_min,
        )
    )

    aggregate_t0 = time.perf_counter()
    aggregated = aggregate_longest_run(
        observations, min_support=max(1, args.min_support)
    )
    aggregate_time_s = time.perf_counter() - aggregate_t0
    scan_time_s = time.perf_counter() - scan_started_at

    print(
        "scan summary "
        f"processed={stats.processed_frames} "
        f"accepted={stats.accepted_count} "
        f"decode_fail={stats.decode_fail_count} "
        f"preamble_fail={stats.preamble_fail_count} "
        f"crc_fail={stats.crc_fail_count} "
        f"min_support={max(1, args.min_support)}"
    )
    print(
        "aggregate summary "
        f"input_observations={len(observations)} "
        f"output_rows={len(aggregated)}"
    )

    if args.profile_times:
        other_time_s = max(
            0.0,
            scan_time_s - stats.barcode_time_s - stats.ocr_time_s - aggregate_time_s,
        )
        print(
            "timing summary "
            f"scan_total_s={scan_time_s:.3f} "
            f"barcode_s={stats.barcode_time_s:.3f} "
            f"ocr_s={stats.ocr_time_s:.3f} "
            f"aggregate_s={aggregate_time_s:.3f} "
            f"other_s={other_time_s:.3f} "
            f"processed_frames={stats.processed_frames} "
            f"workers={args.workers if use_parallel else 1} "
            f"chunk_size={args.chunk_size} "
            f"chunks={chunk_count}"
        )

    if stats.early_eof and end_frame is not None:
        print(
            "warning: reached EOF before requested end-frame "
            f"start_frame={start_frame} end_frame={end_frame}"
        )

    if stats.seek_prestart_skipped_frames > 0:
        print(
            "seek notice: skipped pre-start decoded frames "
            f"count={stats.seek_prestart_skipped_frames}"
        )

    out_path = Path(args.out_csv)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["frame", "sx", "sy", "gx", "gy"])
        for row in aggregated:
            writer.writerow([row.frame, row.sx, row.sy, row.gx, row.gy])

    print(f"wrote {len(aggregated)} rows: {out_path}")

    if args.out_diagnostics_csv:
        diag_path = Path(args.out_diagnostics_csv)
        diag_path.parent.mkdir(parents=True, exist_ok=True)
        with diag_path.open("w", encoding="utf-8", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["frame", "support_len", "confidence_sum"])
            for row in aggregated:
                writer.writerow(
                    [row.frame, row.support_len, f"{row.confidence_sum:.6f}"]
                )
        print(f"wrote diagnostics: {diag_path}")


if __name__ == "__main__":
    main()
