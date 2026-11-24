"""Check a .nup before the build: the right game and update, and every file the build reads intact.

    python3 -I ps5/tools/check_nup.py <oot.nup>

A .nup holds a SHA-256 for its table of contents and for each file in it. This reads the table of contents and every file
the build uses (the program, and the romfs parts the shader list and button prompts are made from) and checks each one
against its own hash. One that fails is read again up to four times: a PC whose memory returns different bytes on two reads
of the same file (a flipped bit; seen on a PC with unstable RAM) passes when a later read is right, and is told so. Then the
game's title and update are checked. Exit 0: fine. Exit 1: the wrong game or update, or a file that never read right.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "nx2"))
import nup as nupmod  # noqa: E402

TITLE_ID = 0x0400486024DEC000
VERSION = 131072   # (1.1.1)
ROMFS_PARTS = ("Shader/", "Layout2Shader/", "Lib/", "Movie/Ndve/", "Effect/", "Pack/ZsDic.pack.zs", "System/Resource/",
               "UI/Layout2Archive/", "Font2/")
TRIES = 5


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_nup.py <oot.nup>")
    path = sys.argv[1]
    flips = 0
    for attempt in range(TRIES):
        try:
            package = nupmod.Nup(path, strict=True)
            break
        except ValueError as error:   # (the table of contents' own hash: read again)
            flips += 1
            if attempt == TRIES - 1:
                print("check: the .nup's table of contents failed its check %d times (%s): the file is damaged" % (TRIES, error))
                return 1
    title = max(package.titles, key=lambda t: t["version"])
    if title["id"] != TITLE_ID or title["version"] != VERSION:
        print("check: this .nup is title %016X v%d; the build needs %016X v%d (Ocarina of Time 1.1.1)" % (
            title["id"], title["version"], TITLE_ID, VERSION))
        return 1
    index = {id(record): i for i, record in enumerate(package.files)}
    members = list(package.verify_walk(title["exefs"]))
    members += [(rel, record) for rel, record in package.verify_walk(title["romfs"]) if rel.startswith(ROMFS_PARTS)]
    total = sum(record[2] for _, record in members)
    checked = 0
    for rel, record in members:
        expected = package.file_details[index[id(record)]][0]
        if expected == bytes(32):
            continue
        for attempt in range(TRIES):
            sink = nupmod.VerifySink()
            package.copy_to(record, sink, rel, packed=record[5] == 1)
            if sink.hash.digest() == expected and sink.size == record[2]:
                break
            flips += 1
            if attempt == TRIES - 1:
                print("check: %s failed its SHA-256 %d times: the .nup is damaged (dump or copy it again)" % (rel, TRIES))
                return 1
        checked += 1
    print("check: title %016X v%d, %d files (%.2f GB) match their hashes" % (title["id"], title["version"], checked,
                                                                             total / 1e9))
    if flips:
        print("check: WARNING: %d read(s) gave different bytes than the file holds and were read again. The file is fine,"
              " but this PC's memory (or drive) is returning wrong bytes now and then: the build can be damaged the same"
              " way. Test the memory (MemTest86) and turn off memory overclocking (XMP/EXPO) before relying on it." % flips)
    return 0


if __name__ == "__main__":
    sys.exit(main())
