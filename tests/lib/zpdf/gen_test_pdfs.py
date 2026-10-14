# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

# Builds small PDFs exercising the parser and writes them as C arrays.
import zlib, sys

def xref_table_pdf(content, compress=False, extra_objs=None):
    objs = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>")
    data = zlib.compress(content) if compress else content
    filt = b"/Filter /FlateDecode " if compress else b""
    objs.append(b"<< " + filt + b"/Length %d >>\nstream\n" % len(data) + data + b"\nendstream")
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, o in enumerate(objs):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n" % (len(objs) + 1)
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)

def xref_stream_pdf(content):
    # objects 1..3 and 5 live inside object stream 6; content is 4; xref stream is 7
    inner = [
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        (3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        (5, b"<< /Type /Font /Subtype /TrueType /BaseFont /Arial /FirstChar 32 /LastChar 126 /Widths " +
            b"[" + b" ".join(b"500" for _ in range(95)) + b"] /Encoding /WinAnsiEncoding >>"),
    ]
    hdr = b""
    body = b""
    for num, o in inner:
        hdr += b"%d %d " % (num, len(body))
        body += o + b"\n"
    stm_data = zlib.compress(hdr + body)
    objstm = b"<< /Type /ObjStm /N %d /First %d /Length %d /Filter /FlateDecode >>\nstream\n" % (len(inner), len(hdr), len(stm_data)) + stm_data + b"\nendstream"
    cdata = zlib.compress(content)
    cont = b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(cdata) + cdata + b"\nendstream"
    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    off6 = len(out); out += b"6 0 obj\n" + objstm + b"\nendobj\n"
    off4 = len(out); out += b"4 0 obj\n" + cont + b"\nendobj\n"
    off7 = len(out)
    # xref stream entries W [1 2 1], Size 8
    rows = []
    rows.append(bytes([0, 0, 0, 255]))                 # 0 free
    for idx, num in enumerate([1, 2, 3]):
        rows.append(bytes([2, 0, 6, idx]))             # in objstm 6
    rows.append(bytes([1, off4 >> 8, off4 & 255, 0]))  # 4
    rows.append(bytes([2, 0, 6, 3]))                   # 5
    rows.append(bytes([1, off6 >> 8, off6 & 255, 0]))  # 6
    rows.append(bytes([1, off7 >> 8, off7 & 255, 0]))  # 7
    # PNG "up" predictor rows to exercise the predictor path
    raw = b"".join(rows)
    prev = bytes(4)
    pred = b""
    for r in rows:
        pred += bytes([2]) + bytes((r[i] - prev[i]) & 255 for i in range(4))
        prev = r
    xdata = zlib.compress(pred)
    out += b"7 0 obj\n<< /Type /XRef /Size 8 /W [1 2 1] /Root 1 0 R /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 4 >> /Length %d >>\nstream\n" % len(xdata) + xdata + b"\nendstream\nendobj\n"
    out += b"startxref\n%d\n%%%%EOF\n" % off7
    return bytes(out)

def type0_pdf():
    tounicode = (b"/CIDInit /ProcSet findresource begin begincmap\n"
                 b"1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                 b"2 beginbfchar <0001> <005A> <0005> <FB01> endbfchar\n"
                 b"1 beginbfrange <0002> <0004> <0065> endbfrange\n"
                 b"endcmap CMapName currentdict /CMap defineresource pop end end")
    objs = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>")
    content = b"BT /F1 12 Tf 20 100 Td <00010002000300040005> Tj ET"
    objs.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
    objs.append(b"<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>")
    objs.append(b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /DW 600 /W [1 [700 500] 3 5 400] >>")
    objs.append(b"<< /Length %d >>\nstream\n" % len(tounicode) + tounicode + b"\nendstream")
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objs):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n" % (len(objs) + 1) + b"0000000000 65535 f \n"
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)

content = (b"BT /F1 14 Tf 20 150 Td (Hello Zephyr) Tj 0 -20 Td [(P) 50 (DF) -600 (reader)] TJ "
           b"0 -20 Td (second line) Tj ET")
plain = xref_table_pdf(content)
flate = xref_stream_pdf(content)
broken = plain.replace(b"startxref\n", b"startxref\n9")   # wrong offset: forces reconstruction
nostartxref = plain[:plain.rfind(b"startxref")]            # truncated tail
t0 = type0_pdf()

def carr(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("\t" + ", ".join("0x%02x" % b for b in data[i:i+16]) + ",")
    return "static const uint8_t %s[] = {\n%s\n};\n" % (name, "\n".join(lines))

with open(sys.argv[1], "w") as f:
    f.write("/* Generated by gen_test_pdfs.py: small documents for the zpdf tests. */\n#include <stdint.h>\n\n")
    f.write(carr("pdf_plain", plain))
    f.write(carr("pdf_xref_stream", flate))
    f.write(carr("pdf_broken_startxref", broken))
    f.write(carr("pdf_truncated", nostartxref))
    f.write(carr("pdf_type0", t0))
for n, d in [("plain", plain), ("xrefstm", flate), ("broken", broken), ("trunc", nostartxref), ("type0", t0)]:
    open(sys.argv[2] + "/" + n + ".pdf", "wb").write(d)
print("ok", len(plain), len(flate), len(t0))
