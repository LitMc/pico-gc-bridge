import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Roi:
    x: int
    y: int
    w: int
    h: int


def load_rois(path: str) -> dict[str, Roi]:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    rois = {}
    for roi in data["rois"]:
        rois[roi["name"]] = Roi(x=roi["x"], y=roi["y"], w=roi["w"], h=roi["h"])
    return rois


def ensure_roi_names(rois: dict[str, Roi], names: list[str]):
    missing = [name for name in names if name not in rois]
    if missing:
        raise ValueError(f"Missing ROI definitions: {missing}")


def crop(frame, roi: Roi):
    return frame[roi.y : roi.y + roi.h, roi.x : roi.x + roi.w]
