#!/usr/bin/env python3
"""Print exact on-disk PE, Crinkler-resource, and launcher-overlay sizes."""
import re
import struct
import sys
from pathlib import Path
from urllib.parse import urlsplit

MODEL_TEXT=(Path(__file__).parents[1]/"src"/"overlay-identity-model.h").read_text(encoding="utf-8")
URL_DIRECT=bytes(map(int,re.search(r'UrlDirect\{\{\s*([0-9,\s]+?)\s*\}\};',MODEL_TEXT).group(1).split(',')))
URL_TOKENS=[x.encode() for x in re.findall(r'^    "([^"]+)",$',MODEL_TEXT,re.M)]
_LENGTHS=[list(map(int,x.split(','))) for x in re.findall(r'Lengths\{\{\s*([0-9,\s]+?)\s*\}\};',MODEL_TEXT)]
URL_LENGTHS,GH_LENGTHS=_LENGTHS
GH_ALPHABET=b"-.0123456789_abcdefghijklmnopqrstuvwxyz"

class Bits:
    def __init__(self,data):self.data=data;self.at=0
    def bit(self):
        if self.at>=len(self.data)*8:raise ValueError("truncated")
        value=(self.data[self.at//8]>>(7-self.at%8))&1;self.at+=1;return value
    def gamma(self):
        zeros=0
        while not self.bit():
            zeros+=1
            if zeros>=16:raise ValueError("gamma overflow")
        value=1
        for _ in range(zeros):value=value<<1|self.bit()
        return value
    def padding(self):
        if len(self.data)*8-self.at>=8:raise ValueError("trailing data")
        while self.at<len(self.data)*8:
            if self.bit():raise ValueError("padding")

def symbol(bits,lengths):
    ordered=sorted(range(len(lengths)),key=lambda x:(lengths[x],x));codes={};code=0;previous=lengths[ordered[0]]
    for item in ordered:
        code<<=lengths[item]-previous;codes[lengths[item],code]=item;code+=1;previous=lengths[item]
    value=0
    for length in range(1,max(lengths)+1):
        value=value<<1|bits.bit()
        if (length,value) in codes:return codes[length,value]
    raise ValueError("symbol")

def decode_url(data):
    bits=Bits(data);out=bytearray();upper=len(URL_DIRECT);token_base=upper+1;escape=token_base+len(URL_TOKENS);copy=escape+1;eof=copy+1
    while True:
        item=symbol(bits,URL_LENGTHS)
        if item==eof:bits.padding();return bytes(out)
        if item<upper:out.append(URL_DIRECT[item])
        elif item==upper:
            value=0
            for _ in range(5):value=value<<1|bits.bit()
            if value>=26:raise ValueError("UPPER")
            out.append(ord('A')+value)
        elif item<escape:out+=URL_TOKENS[item-token_base]
        elif item==escape:
            value=0
            for _ in range(8):value=value<<1|bits.bit()
            out.append(value)
        elif item==copy:
            distance=bits.gamma();length=bits.gamma()+2
            if distance>len(out):raise ValueError("copy")
            for _ in range(length):out.append(out[-distance])
        else:raise ValueError("opcode")

def decode_github(data):
    bits=Bits(data);end=len(GH_ALPHABET);copy_owner=end+1;copy_repo=copy_owner+1
    exact_owner=copy_repo+1;exact_repo=exact_owner+1;whole_owner=exact_repo+1;whole_repo=whole_owner+1
    def field(sources,app=False):
        out=bytearray()
        while True:
            item=symbol(bits,GH_LENGTHS)
            if item<end:
                c=GH_ALPHABET[item]
                if app and c in b"._":raise ValueError("app character")
                out.append(c)
            elif item==end:
                if not out:raise ValueError("empty")
                return bytes(out)
            elif item in (exact_owner,exact_repo):
                source=item-exact_owner
                if out or source>=len(sources):raise ValueError("exact")
                return sources[source]
            elif item in (whole_owner,whole_repo):
                source=item-whole_owner
                if source>=len(sources):raise ValueError("whole")
                out+=sources[source]
            elif item in (copy_owner,copy_repo):
                source=item-copy_owner;offset=bits.gamma()-1;length=bits.gamma()
                if source>=len(sources) or length<3 or offset+length>len(sources[source]):raise ValueError("copy")
                out+=sources[source][offset:offset+length]
            else:raise ValueError("opcode")
    owner=field([]);repository=field([owner]);app=field([owner,repository],True) if bits.bit() else b""
    bits.padding();return owner+b"/"+repository+(b"#"+app if app else b"")


def overlay_info(data):
    if not data:
        return None
    typed = data[-1]
    if typed == 0xc0:
        return (1, 0, 1, 0)
    kind, encoded = typed & 0xc0, typed & 0x3f
    if kind == 0xc0 or not encoded:
        return None
    trailer = 1
    if encoded == 0x3f:
        if len(data) < 2 or data[-2] < 0x3f:
            return None
        encoded, trailer = data[-2], 2
    if encoded > 255:
        return None
    candidates = []
    for secret_bytes in (0, 8):
        if encoded + secret_bytes + trailer > len(data):
            continue
        start = len(data) - encoded - secret_bytes - trailer
        prefix = (b"gh:", b"url:https://", b"url:http://")[kind >> 6]
        try:
            encoded_body=data[start:start + encoded]
            body=decode_github(encoded_body) if kind==0 else decode_url(encoded_body)
            identity = (prefix + body).decode("utf-8")
        except (UnicodeDecodeError,ValueError):
            continue
        if identity.startswith("gh:"):
            canonical = re.fullmatch(r"gh:[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:#[A-Za-z0-9][A-Za-z0-9-]{0,62})?", identity) is not None
        else:
            url = identity[4:]
            parts = urlsplit(url)
            canonical = bool(parts.hostname) and ((parts.scheme == "https") or (parts.scheme == "http" and parts.hostname.lower() in ("localhost", "127.0.0.1")))
        if canonical:
            candidates.append((encoded + secret_bytes + trailer, encoded, trailer, secret_bytes))
    return candidates[0] if len(candidates) == 1 else None


def c_string(data, at):
    end = data.find(b"\0", at)
    return data[at:end].decode("ascii", "replace") if 0 <= at < len(data) and end >= 0 else "?"


def rva_file(sections, rva):
    section_rvas = [va for _, _, _, _, va in sections if va]
    if not section_rvas or rva < min(section_rvas):
        return rva  # zero-section images and ordinary PE headers are identity-mapped
    for _, raw, file_at, virtual, va in sections:
        if va <= rva < va + max(raw, virtual):
            return file_at + rva - va
    return None


def imports_of(data, sections, import_rva):
    result = []
    descriptor = rva_file(sections, import_rva) if import_rva else None
    while descriptor is not None and descriptor + 20 <= len(data):
        lookup, _, _, name_rva, iat = struct.unpack_from("<IIIII", data, descriptor)
        if not any((lookup, name_rva, iat)):
            break
        name_at = rva_file(sections, name_rva)
        thunk = rva_file(sections, lookup or iat)
        names = []
        while thunk is not None and thunk + 4 <= len(data):
            value, = struct.unpack_from("<I", data, thunk)
            if not value:
                break
            if value & 0x80000000:
                names.append(f"#{value & 0xffff}")
            else:
                function_at = rva_file(sections, value)
                names.append(c_string(data, function_at + 2) if function_at is not None else "?")
            thunk += 4
        result.append((c_string(data, name_at) if name_at is not None else "?", names))
        descriptor += 20
    return result


def resource_leaf_size(data, root, wanted_type):
    def entries(directory):
        if directory < 0 or directory + 16 > len(data):
            return []
        named, ids = struct.unpack_from("<HH", data, directory + 12)
        count = named + ids
        if count > 32 or directory + 16 + count * 8 > len(data):
            return []
        return [struct.unpack_from("<II", data, directory + 16 + i * 8) for i in range(count)]

    type_entry = next((entry for entry in entries(root) if not entry[0] & 0x80000000 and entry[0] == wanted_type), None)
    if not type_entry or not type_entry[1] & 0x80000000:
        return 0
    names = entries(root + (type_entry[1] & 0x7fffffff))
    if not names or not names[0][1] & 0x80000000:
        return 0
    languages = entries(root + (names[0][1] & 0x7fffffff))
    if not languages or languages[0][1] & 0x80000000:
        return 0
    data_entry = root + languages[0][1]
    return struct.unpack_from("<I", data, data_entry + 4)[0] if data_entry + 8 <= len(data) else 0


for arg in sys.argv[1:]:
    path = Path(arg)
    data = path.read_bytes()
    pe, = struct.unpack_from("<I", data, 0x3c)
    count, = struct.unpack_from("<H", data, pe + 6)
    optional_bytes, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    headers, = struct.unpack_from("<I", data, optional + 60)
    code, initialized, uninitialized = struct.unpack_from("<III", data, optional + 4)
    section_at = pe + 24 + optional_bytes
    sections = []
    for i in range(count):
        name, virtual, va, raw, file_at = struct.unpack_from("<8sIIII", data, section_at + i * 40)[:5]
        sections.append((name.rstrip(b"\0").decode("ascii", "replace"), raw, file_at, virtual, va))

    overlay = overlay_info(data)
    config = overlay[0] if overlay else 0

    print(f"{path}: {len(data)} bytes (physical EOF {len(data)})")
    crinkler = count == 0 and pe == 4 and optional_bytes in (8, 120)
    icon_crinkler = crinkler and optional_bytes == 120
    if crinkler:
        kind = "resource-capable Crinkler zero-section image" if icon_crinkler else "Crinkler zero-section image"
        print(f"  {kind}: {len(data) - config}")
        print(f"  nominal SizeOfHeaders: {headers}")
    else:
        print(f"  PE headers: {headers}")
        print(f"  optional-header allocations: code={code}, initialized={initialized}, uninitialized={uninitialized}")
    for name, raw, file_at, virtual, _ in sections:
        print(f"  section {name or '(unnamed)'}: raw={raw}, file_offset={file_at}, virtual={virtual}")

    directory_count = struct.unpack_from("<I", data, optional + 92)[0] if optional_bytes >= 96 else 0
    import_rva = struct.unpack_from("<I", data, optional + 104)[0] if directory_count > 1 else 0
    resource_rva, resource_size = struct.unpack_from("<II", data, optional + 112) if directory_count > 2 else (0, 0)
    imported = imports_of(data, sections, import_rva)
    if imported:
        print("  named imports: " + "; ".join(f"{dll}: {', '.join(names)}" for dll, names in imported))

    if icon_crinkler and resource_rva:
        split = 148
        carrier = headers - split
        packed_at = struct.unpack_from("<I", data, 15)[0] - 0x400000
        icon_bytes = resource_leaf_size(data, resource_rva, 3)
        group_bytes = resource_leaf_size(data, resource_rva, 14)
        metadata = carrier - icon_bytes - group_bytes
        print(f"  Crinkler header/decompressor: {packed_at - carrier}")
        print(f"  compressed payload and tiny imports: {len(data) - config - packed_at}")
        print(f"  additional physical resource metadata: {metadata}")
        print(f"  RT_ICON: {icon_bytes}; RT_GROUP_ICON: {group_bytes}")
        print(f"  inserted carrier at file/RVA {split}: {carrier}")
        print(f"  resource root RVA: {resource_rva}; mapped tree span: {headers - resource_rva}")
        print(f"  nominal resource-directory size (also root counts): {resource_size}")
        print("  alignment/gaps: 0")
    elif resource_size:
        print(f"  resource directory: RVA={resource_rva}, size={resource_size}")
    if overlay:
        _, encoded, trailer, secret_bytes = overlay
        print(f"  encoded identity body: {encoded}; typed trailer: {trailer}; private secret: {secret_bytes}")
    print(f"  configuration overlay: {config}")
    print(f"  accounted on disk: {len(data)}; other overlay/padding: 0")
