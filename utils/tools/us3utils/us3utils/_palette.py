"""Small validated color palette for the ASTFVM figures (subset of the
project's dataviz reference palette: fixed-order categorical slots and a
single-hue sequential blue ramp for ordered quantities like N or dt)."""

# Fixed-order categorical slots (light-mode, validated for CVD contrast).
CAT_BLUE = "#2a78d6"    # slot 1: N-sweep series
CAT_RED = "#e34948"     # slot 6: dt-sweep series
CAT_GREEN = "#008300"   # slot 4: reference / analytic curve
CAT_VIOLET = "#4a3aa7"  # slot 5
CAT_ORANGE = "#eb6834"  # slot 8
CAT_AQUA = "#1baf7a"    # slot 2

# Fixed-order palette used to assign colors to an open-ended list of mesh
# configuration "series" (e.g. refine/uniform combinations), by sorted
# series-name order -- never re-assigned per data value.
CAT_SERIES_ORDER = [CAT_BLUE, CAT_RED, CAT_VIOLET, CAT_ORANGE, CAT_GREEN, CAT_AQUA]
MARKER_SERIES_ORDER = ["o", "s", "^", "D", "v", "P"]

# Chart chrome
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
SURFACE = "#fcfcfb"

# Sequential blue ramp, light -> dark (step 150 .. 650), for ordered
# quantities (e.g. increasing N or decreasing dt).
SEQ_BLUE_RAMP = [
    "#b7d3f6", "#9ec5f4", "#86b6ef", "#6da7ec", "#5598e7",
    "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95", "#104281",
]


def sequential_blues(n: int) -> list:
    """Return n colors sampled evenly along the sequential blue ramp."""
    if n <= 1:
        return [SEQ_BLUE_RAMP[len(SEQ_BLUE_RAMP) // 2]]
    step = (len(SEQ_BLUE_RAMP) - 1) / (n - 1)
    return [SEQ_BLUE_RAMP[round(i * step)] for i in range(n)]


def sequential_blue_cmap():
    """A matplotlib colormap built from the sequential blue ramp."""
    from matplotlib.colors import LinearSegmentedColormap

    return LinearSegmentedColormap.from_list("us3_seq_blue", SEQ_BLUE_RAMP)
