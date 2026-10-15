"""Comparison of compiled functions (.o) against functions of main and
sequence alignment to assign names by link order."""
from __future__ import annotations

import difflib

import numpy as np

from common import words, normalize_linked


def compare(objfn, target_bytes: bytes):
    """Return (exact, similarity 0..1).

    exact: same size and same bits outside the fields with relocations
    (masked on both sides according to the .o relocations).
    similarity: difflib ratio over normalized words (.o relocations
    masked; on the target, heuristic normalization)."""
    a = objfn.masked()
    tw = words(target_bytes)
    if len(tw) == len(a):
        b = list(tw)
        for off, (rtype, _s) in objfn.relocs.items():
            from common import RELOC_MASK
            m = RELOC_MASK.get(rtype)
            if m is not None:
                b[off // 4] &= ~m
        if b == a:
            return True, 1.0
    # approximate similarity with both heuristically normalized
    na = normalize_linked(words(objfn.data))
    for off, (rtype, _s) in objfn.relocs.items():
        # in the .o unresolved immediates are 0; normalize_linked already
        # masks BL/ADRP/LO12, so that is enough
        pass
    nb = normalize_linked(tw)
    sm = difflib.SequenceMatcher(None, na, nb, autojunk=False)
    return False, sm.ratio()


def _hist(ws):
    h = np.zeros(512, dtype=np.float32)
    for w in ws:
        h[(w >> 23) & 0x1FF] += 1
    n = np.linalg.norm(h)
    return h / n if n else h


def align(objfns, targets, img, min_sim: float = 0.35):
    """Global alignment (Needleman-Wunsch style without gap penalty)
    between the list of compiled functions (link order) and the functions
    of the target (address order). Returns [(i_obj, j_target, sim)]."""
    n, m = len(objfns), len(targets)
    A = np.stack([_hist(words(f.data)) for f in objfns])
    tb = [img.read(a, s) for a, s, _ in targets]
    B = np.stack([_hist(words(b)) for b in tb])
    cos = A @ B.T
    sa = np.array([f.size for f in objfns], dtype=np.float32)[:, None]
    sb = np.array([s for _, s, _ in targets], dtype=np.float32)[None, :]
    ratio = np.minimum(sa, sb) / np.maximum(np.maximum(sa, sb), 1)
    sim = cos * np.sqrt(ratio)
    sim[sim < min_sim] = -1.0
    # DP
    D = np.zeros((n + 1, m + 1), dtype=np.float32)
    for i in range(1, n + 1):
        row = sim[i - 1]
        prev = D[i - 1]
        cur = D[i]
        for j in range(1, m + 1):
            best = prev[j]
            if cur[j - 1] > best:
                best = cur[j - 1]
            s = row[j - 1]
            if s > 0 and prev[j - 1] + s > best:
                best = prev[j - 1] + s
            cur[j] = best
    # backtrack
    pairs = []
    i, j = n, m
    while i > 0 and j > 0:
        s = sim[i - 1, j - 1]
        if s > 0 and abs(D[i, j] - (D[i - 1, j - 1] + s)) < 1e-4:
            pairs.append((i - 1, j - 1, float(s)))
            i -= 1
            j -= 1
        elif D[i, j] == D[i - 1, j]:
            i -= 1
        else:
            j -= 1
    pairs.reverse()
    return pairs, tb
