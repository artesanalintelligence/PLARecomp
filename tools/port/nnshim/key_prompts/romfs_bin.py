"""Minimal romfs.bin reader (Switch RomFS, level 3) in pure Python, for the tools that used to read
the extracted folder (key_prompts/gen_key_prompts.py). Same format and same rules as nnshim/romfs.hpp: only
the metadata tables are read, the index is case-insensitive and data is read by offset.

    img = RomfsBin("D:/PLARecomp/dist/PLA-PC-v0/exefs/romfs.bin")
    for path in img.walk("bin/appli", ".arc"):   # order of glob(..., "**", "*.arc") over the folder on NTFS
        data = img.read(path)
"""
import struct

NONE = 0xFFFFFFFF


class RomfsBin:
    def __init__(self, path: str):
        self.path = path
        self._f = open(path, "rb")
        hdr = struct.unpack("<10Q", self._f.read(0x50))
        if hdr[0] != 0x50:
            raise ValueError(f"{path}: not a RomFS image")
        _, _, _, dmo, dms, _, _, fmo, fms, self.data_off = hdr
        self._f.seek(dmo)
        dm = self._f.read(dms)
        self._f.seek(fmo)
        fm = self._f.read(fms)
        # directory: (name, [subdirs], [files]); file: (name, absolute offset, size)
        self.dirs = {}      # lowercase path -> (path, [subdirectory paths], [file paths])
        self.files = {}     # lowercase path -> (path, offset, size)
        stack = [(0, "")]
        seen = {0}
        while stack:
            off, path_ = stack.pop()
            subdirs, files = [], []
            c = struct.unpack_from("<I", dm, off + 8)[0]
            while c != NONE:
                if c in seen:
                    raise ValueError("corrupt directory table")
                seen.add(c)
                nlen = struct.unpack_from("<I", dm, c + 0x14)[0]
                name = dm[c + 0x18:c + 0x18 + nlen].decode("utf-8")
                sub = f"{path_}/{name}" if path_ else name
                subdirs.append(sub)
                stack.append((c, sub))
                c = struct.unpack_from("<I", dm, c + 4)[0]
            fo = struct.unpack_from("<I", dm, off + 0xC)[0]
            while fo != NONE:
                _, sib, doff, size, _, nlen = struct.unpack_from("<IIQQII", fm, fo)
                name = fm[fo + 0x20:fo + 0x20 + nlen].decode("utf-8")
                fp = f"{path_}/{name}" if path_ else name
                files.append(fp)
                self.files[fp.lower()] = (fp, self.data_off + doff, size)
                fo = sib
            self.dirs[path_.lower()] = (path_, subdirs, files)

    @staticmethod
    def _norm(p: str) -> str:
        return "/".join(x for x in p.replace("\\", "/").split("/") if x and x != ".").lower()

    def is_file(self, p: str) -> bool:
        return self._norm(p) in self.files

    def size(self, p: str) -> int:
        return self.files[self._norm(p)][2]

    def read(self, p: str, offset: int = 0, size: int = -1) -> bytes:
        _, off, total = self.files[self._norm(p)]
        if size < 0 or offset + size > total:
            size = max(0, total - offset)
        self._f.seek(off + offset)
        return self._f.read(size)

    def walk(self, top: str = "", suffix: str = ""):
        """Files under `top` (recursive) ending in `suffix` (case-insensitive), in the order that
        glob.glob(os.path.join(folder, top, "**", "*" + suffix), recursive=True) gives them over the extracted
        folder on NTFS: directories in preorder, siblings and files sorted by their uppercase name."""
        key = self._norm(top)
        if key not in self.dirs:
            return
        stack = [key]
        while stack:
            path_, subdirs, files = self.dirs[stack.pop()]
            for fp in sorted(files, key=lambda x: x.rsplit("/", 1)[-1].upper()):
                if fp.lower().endswith(suffix.lower()):
                    yield fp
            for sd in sorted(subdirs, key=lambda x: x.rsplit("/", 1)[-1].upper(), reverse=True):
                stack.append(sd.lower())

    def close(self):
        self._f.close()
