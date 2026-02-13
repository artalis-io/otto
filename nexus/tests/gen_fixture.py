#!/usr/bin/env python3
"""Generate minimal XLSX test fixture for Nexus ingestion tests.

Creates a valid XLSX (ZIP of XML files) with known content for deterministic testing.
"""

import zipfile
import os
import io

CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>"""

WORKBOOK_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>
</Relationships>"""

WORKBOOK = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheets>
    <sheet name="Locations" sheetId="1" r:id="rId1" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"/>
  </sheets>
</workbook>"""

# Shared strings: City, Name, Address, Budapest, Depot #1, Futó u. 35-37,
#                  Debrecen, Depot #2, Balmazújvárosi út
SHARED_STRINGS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="11" uniqueCount="11">
  <si><t>City</t></si>
  <si><t>Name</t></si>
  <si><t>Address</t></si>
  <si><t>GPS Lat</t></si>
  <si><t>GPS Lon</t></si>
  <si><t>Budapest</t></si>
  <si><t>Depot #1</t></si>
  <si><t>Futó u. 35-37</t></si>
  <si><t>Debrecen</t></si>
  <si><t>Depot #2</t></si>
  <si><t>Balmazújvárosi út</t></si>
</sst>"""

# Sheet1 layout:
# Row 1: City(s0) | Name(s1) | Address(s2) | GPS Lat(s3) | GPS Lon(s4)
# Row 2: Budapest(s5) | Depot #1(s6) | Futó u. 35-37(s7) | 47.4799 | 19.0700
# Row 3: Debrecen(s8) | Depot #2(s9) | Balmazújvárosi út(s10) | 47.5316 | 21.6273
SHEET1 = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheetData>
    <row r="1">
      <c r="A1" t="s"><v>0</v></c>
      <c r="B1" t="s"><v>1</v></c>
      <c r="C1" t="s"><v>2</v></c>
      <c r="D1" t="s"><v>3</v></c>
      <c r="E1" t="s"><v>4</v></c>
    </row>
    <row r="2">
      <c r="A2" t="s"><v>5</v></c>
      <c r="B2" t="s"><v>6</v></c>
      <c r="C2" t="s"><v>7</v></c>
      <c r="D2"><v>47.4799</v></c>
      <c r="E2"><v>19.07</v></c>
    </row>
    <row r="3">
      <c r="A3" t="s"><v>8</v></c>
      <c r="B3" t="s"><v>9</v></c>
      <c r="C3" t="s"><v>10</v></c>
      <c r="D3"><v>47.5316</v></c>
      <c r="E3"><v>21.6273</v></c>
    </row>
  </sheetData>
</worksheet>"""

def create_xlsx(output_path):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('[Content_Types].xml', CONTENT_TYPES)
        zf.writestr('_rels/.rels', RELS)
        zf.writestr('xl/_rels/workbook.xml.rels', WORKBOOK_RELS)
        zf.writestr('xl/workbook.xml', WORKBOOK)
        zf.writestr('xl/sharedStrings.xml', SHARED_STRINGS)
        zf.writestr('xl/worksheets/sheet1.xml', SHEET1)

    data = buf.getvalue()
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(data)
    print(f"Created {output_path} ({len(data)} bytes)")

    # Also create a C header with the bytes embedded
    header_path = output_path.replace('.xlsx', '_xlsx.h')
    with open(header_path, 'w') as f:
        f.write("/* Auto-generated minimal XLSX fixture */\n")
        f.write(f"static const unsigned char MINIMAL_XLSX[] = {{\n")
        for i, b in enumerate(data):
            if i % 16 == 0:
                f.write("    ")
            f.write(f"0x{b:02x},")
            if i % 16 == 15:
                f.write("\n")
            else:
                f.write(" ")
        f.write("\n};\n")
        f.write(f"static const size_t MINIMAL_XLSX_LEN = {len(data)};\n")
    print(f"Created {header_path}")

if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))
    create_xlsx(os.path.join(script_dir, 'fixtures', 'minimal.xlsx'))
