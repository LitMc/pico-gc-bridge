from dataclasses import dataclass


@dataclass(frozen=True)
class ObservationRow:
    video_frame_idx: int
    raw_frame_id: int
    sx: int
    sy: int
    gx: int
    gy: int
    conf_min: float


@dataclass(frozen=True)
class AggregatedRow:
    frame: int
    sx: int
    sy: int
    gx: int
    gy: int
    support_len: int
    confidence_sum: float


def aggregate_longest_run(rows: list[ObservationRow], min_support=1):
    grouped: dict[int, list[ObservationRow]] = {}
    for row in rows:
        grouped.setdefault(row.raw_frame_id, []).append(row)

    out: list[AggregatedRow] = []

    for raw_frame_id, group in sorted(grouped.items()):
        seq = sorted(group, key=lambda r: r.video_frame_idx)
        if not seq:
            continue

        best_key = None
        best_len = -1
        best_conf = -1.0

        run_key = (seq[0].sx, seq[0].sy, seq[0].gx, seq[0].gy)
        run_len = 1
        run_conf = seq[0].conf_min
        prev_idx = seq[0].video_frame_idx

        def flush_current(cur_key, cur_len, cur_conf):
            nonlocal best_key, best_len, best_conf
            if cur_len > best_len:
                best_key = cur_key
                best_len = cur_len
                best_conf = cur_conf
            elif cur_len == best_len and cur_conf > best_conf:
                best_key = cur_key
                best_len = cur_len
                best_conf = cur_conf

        for row in seq[1:]:
            key = (row.sx, row.sy, row.gx, row.gy)
            contiguous = row.video_frame_idx == prev_idx + 1
            if key == run_key and contiguous:
                run_len += 1
                run_conf += row.conf_min
            else:
                flush_current(run_key, run_len, run_conf)
                run_key = key
                run_len = 1
                run_conf = row.conf_min
            prev_idx = row.video_frame_idx

        flush_current(run_key, run_len, run_conf)
        if best_key is None or best_len < min_support:
            continue

        out.append(
            AggregatedRow(
                frame=raw_frame_id,
                sx=best_key[0],
                sy=best_key[1],
                gx=best_key[2],
                gy=best_key[3],
                support_len=best_len,
                confidence_sum=best_conf,
            )
        )

    return out
