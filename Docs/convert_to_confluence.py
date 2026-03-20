import sys, os, re

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

script_dir = os.path.dirname(os.path.abspath(__file__))
input_path = os.path.join(script_dir, 'LevelTool_DesignerIntent_보완문서.md')
output_html = os.path.join(script_dir, 'LevelTool_DesignerIntent_보완문서_Confluence.html')
output_xml = os.path.join(script_dir, 'LevelTool_DesignerIntent_보완문서_Confluence.xml')

with open(input_path, 'r', encoding='utf-8') as f:
    md = f.read()

import markdown

def pre_process(text):
    def replace_code_block(m):
        lang = m.group(1) or ''
        code = m.group(2)
        code_escaped = code.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        return (f'<ac:structured-macro ac:name="code">'
                f'<ac:parameter ac:name="language">{lang}</ac:parameter>'
                f'<ac:plain-text-body><![CDATA[{code}]]></ac:plain-text-body>'
                f'</ac:structured-macro>')
    text = re.sub(r'```(\w*)\n(.*?)```', replace_code_block, text, flags=re.DOTALL)

    def replace_blockquote(m):
        content = m.group(0)
        lines = content.split('\n')
        inner = '\n'.join(l.lstrip('> ').lstrip('>') for l in lines)
        return (f'<ac:structured-macro ac:name="info">'
                f'<ac:rich-text-body>{inner}</ac:rich-text-body>'
                f'</ac:structured-macro>')
    text = re.sub(r'(?:^> .*\n?)+', replace_blockquote, text, flags=re.MULTILINE)

    toc_pattern = r'## 목차\n.*?(?=\n---|\n## )'
    toc_replacement = '<ac:structured-macro ac:name="toc"><ac:parameter ac:name="maxLevel">3</ac:parameter></ac:structured-macro>\n'
    text = re.sub(toc_pattern, toc_replacement, text, flags=re.DOTALL)

    return text

processed = pre_process(md)
html_body = markdown.markdown(processed, extensions=['tables', 'fenced_code'])
html_body = html_body.replace('<hr />', '<hr/>')

html_full = f"""<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8"/>
<title>Designer Intent System — 기획 보완문서</title>
<style>
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; max-width: 960px; margin: 0 auto; padding: 20px; line-height: 1.6; color: #172B4D; }}
table {{ border-collapse: collapse; width: 100%; margin: 16px 0; }}
th, td {{ border: 1px solid #DFE1E6; padding: 8px 12px; text-align: left; }}
th {{ background: #F4F5F7; font-weight: 600; }}
tr:nth-child(even) {{ background: #FAFBFC; }}
h1 {{ color: #172B4D; border-bottom: 2px solid #0052CC; padding-bottom: 8px; }}
h2 {{ color: #172B4D; border-bottom: 1px solid #DFE1E6; padding-bottom: 6px; margin-top: 32px; }}
h3 {{ color: #42526E; margin-top: 24px; }}
code {{ background: #F4F5F7; padding: 2px 6px; border-radius: 3px; font-size: 0.9em; }}
pre {{ background: #F4F5F7; padding: 16px; border-radius: 3px; overflow-x: auto; }}
blockquote, ac\\:structured-macro[ac\\:name="info"] {{ border-left: 4px solid #0052CC; padding: 12px 16px; margin: 16px 0; background: #DEEBFF; }}
hr {{ border: none; border-top: 1px solid #DFE1E6; margin: 24px 0; }}
</style>
</head>
<body>
{html_body}
</body>
</html>"""

with open(output_html, 'w', encoding='utf-8') as f:
    f.write(html_full)

xml_body = html_body
xml_full = f"""<?xml version="1.0" encoding="UTF-8"?>
<ac:confluence xmlns:ac="http://atlassian.com/content" xmlns:ri="http://atlassian.com/resource-identifier">
{xml_body}
</ac:confluence>"""

with open(output_xml, 'w', encoding='utf-8') as f:
    f.write(xml_full)

print(f"HTML: {output_html}")
print(f"XML:  {output_xml}")
print("Done.")
