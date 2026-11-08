#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the printable quick start. Requires reportlab and markdown."""
from pathlib import Path
import html, re
import markdown
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, KeepTogether
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.colors import HexColor
from reportlab.lib.enums import TA_LEFT
root = Path(__file__).resolve().parents[1]
text = (root / 'docs/quick-start.md').read_text()
css = '''body{font:17px/1.55 system-ui,sans-serif;color:#202a35;max-width:850px;margin:40px auto;padding:0 24px}h1,h2{line-height:1.2}h1{color:#17657d}h2{margin-top:32px}li{margin:8px 0}a{color:#17657d}@media print{body{font-size:11pt;margin:0}h2{break-after:avoid}li{break-inside:avoid}}'''
(root/'docs/quick-start.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>QLooper RC1 - Quick start</title><style>'+css+'</style><body>'+markdown.markdown(text)+'</body></html>')
styles=getSampleStyleSheet()
styles.add(ParagraphStyle(name='Copy',fontName='Helvetica',fontSize=10,leading=13,spaceAfter=5,textColor=HexColor('#202a35')))
styles['Heading1'].fontSize=23;styles['Heading1'].leading=27;styles['Heading1'].textColor=HexColor('#17657d')
styles['Heading2'].keepWithNext=True;styles['Heading2'].fontSize=13;styles['Heading2'].leading=17;styles['Heading2'].spaceBefore=11

def inline(s):
 s=s.replace('→',' > ').replace('…','...').replace('—','-').replace('–','-')
 s=html.escape(s)
 s=re.sub(r'\*\*(.*?)\*\*',r'<b>\1</b>',s)
 s=re.sub(r'\[([^\]]+)\]\(([^)]+)\)',r'<link href="\2" color="#17657d">\1</link>',s)
 return s
flow=[]
for block in text.split('\n\n'):
 lines=block.splitlines()
 if not lines: continue
 if block.startswith('# '):flow.append(Paragraph(inline(block[2:]),styles['Heading1']));continue
 if block.startswith('## '):flow.append(Paragraph(inline(block[3:]),styles['Heading2']));continue
 entries=[]
 for line in lines:
  if re.match(r'^(\d+\. |\- )',line):entries.append(line)
  elif entries:entries[-1]+=' '+line.strip()
  else:entries.append(line)
 for entry in entries:
  flow.append(Paragraph(inline(entry),styles['Copy']))
 flow.append(Spacer(1,2))
def footer(c,d):
 c.setFont('Helvetica',8);c.setFillColor(HexColor('#62717c'));c.drawString(42,25,'QLooper 1.0.0-rc1 | qlooper.princ.biz');c.drawRightString(553,25,str(d.page))
SimpleDocTemplate(str(root/'docs/QLooper-Quick-Start.pdf'),pagesize=(595,842),rightMargin=42,leftMargin=42,topMargin=38,bottomMargin=42,title='QLooper RC1 - Quick start',author='QLooper').build(flow,onFirstPage=footer,onLaterPages=footer)
