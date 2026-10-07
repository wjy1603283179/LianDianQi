"""Create template crops and a clickable acceptance page from the supplied image."""
import argparse
import json
import base64
from pathlib import Path
from PIL import Image

BOXES = [
    ('大哭',57,31,156,80),('张嘴',178,31,274,80),('三条',288,30,383,80),
    ('泪痕',471,30,564,83),('游戏',577,29,676,80),('放在',693,49,791,101),
    ('风格',56,109,156,162),('蓝绿色',206,108,351,156),('闭眼',366,103,460,153),
    ('抽烟',485,86,579,138),('根本',588,82,687,134),('动漫',689,111,825,181),
    ('五名',81,166,179,215),('还子',188,162,271,205),('媒体',277,159,375,212),
    ('马尾',383,151,480,203),('胸前',539,139,637,190),('戴猫',108,216,209,266),
    ('卡通',258,216,359,266),('没有',398,206,478,249),('威胁',494,192,591,242),
    ('脸颊',663,196,763,248),('这种',24,256,106,302),('女孩',384,250,480,302),
    ('耳机',492,251,590,304),('握拳',608,250,706,302),('后头',741,249,822,295),
    ('主体',91,301,202,362),('理论',244,282,342,334),('Q版',545,309,638,363),
    ('评论',714,297,823,359),('双手',209,337,308,390),('社交',317,332,427,395),
    ('意识',434,341,530,397),('排列',652,352,735,400),('正文',740,352,822,400),
]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,required=True)
    args=parser.parse_args()
    destination=Path(__file__).resolve().parents[1]/'examples'/'image-matching'
    (destination/'templates').mkdir(parents=True,exist_ok=True)
    image=Image.open(args.source).convert('RGB')
    image.save(destination/'words.png')
    background=image.getpixel((0,0))
    words=[]
    for i,(word,x0,y0,x1,y1) in enumerate(BOXES,1):
        points=[(x,y) for y in range(y0,y1) for x in range(x0,x1)
                if max(abs(a-b) for a,b in zip(image.getpixel((x,y)),background))>35]
        left=max(x0,min(x for x,y in points)-2); top=max(y0,min(y for x,y in points)-2)
        right=min(x1,max(x for x,y in points)+3); bottom=min(y1,max(y for x,y in points)+3)
        file=f'templates/{i:02d}.png'
        image.crop((left,top,right,bottom)).save(destination/file)
        words.append(dict(id=i,word=word,x=left,y=top,width=right-left,height=bottom-top,file=file))
    (destination/'words.json').write_text(json.dumps(words,ensure_ascii=False,indent=2),encoding='utf-8')
    steps=[]
    for w in words:
        steps.extend([dict(action='ifImage',templateName=w['word'],templatePng=base64.b64encode((destination/w['file']).read_bytes()).decode(),similarity=88,scaleMatch=True,limitRegion=False),dict(action='clickMatch'),dict(action='endIf')])
    (destination/'click-all-words.json').write_text(json.dumps(dict(format='liandianqi-script',version=1,rounds=1,startDelayMs=3000,steps=steps),ensure_ascii=False,indent=2),encoding='utf-8')
    (destination/'if-else-example.json').write_text(json.dumps(dict(format='liandianqi-script',version=1,rounds=1,startDelayMs=3000,steps=[steps[0],steps[1],dict(action='else'),dict(action='wait',durationMs=300),steps[2]]),ensure_ascii=False,indent=2),encoding='utf-8')
    links='\n'.join(f'<a href="#word-{w["id"]}" data-id="{w["id"]}" data-word="{w["word"]}"><rect x="{w["x"]}" y="{w["y"]}" width="{w["width"]}" height="{w["height"]}" fill="transparent"/><title>{w["word"]}</title></a>' for w in words)
    html='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>点序图像匹配验收</title>
<style>body{font:16px "Microsoft YaHei",sans-serif;background:#f5f6fa;color:#25304a;margin:24px}h1{font-size:24px}button,select{padding:8px;border:1px solid #dce1ec;border-radius:8px;background:white}svg{display:block;margin:20px 0;background:#f7f8fd;max-width:none}a:hover rect{stroke:#6371dc;stroke-width:2}#log{max-width:1000px;line-height:1.8}#count{color:#5968dc}</style>
<h1>图像匹配验收</h1><label>显示比例 <select id="scale"><option>.5</option><option>.75</option><option selected>1</option><option>1.25</option><option>1.5</option><option>2</option></select></label> <button id="reset">清空记录</button> <span id="count">0 / 36</span>
<svg id="canvas" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 WIDTH HEIGHT" width="WIDTH" height="HEIGHT"><image href="words.png" width="WIDTH" height="HEIGHT"/>LINKS</svg>
<div id="log"></div><script>
const canvas=document.querySelector('#canvas'), log=document.querySelector('#log'), seen=new Set();
canvas.addEventListener('click',e=>{const a=e.target.closest('a');if(!a)return;e.preventDefault();seen.add(a.dataset.id);log.textContent+=(log.textContent?' · ':'')+a.dataset.word;document.querySelector('#count').textContent=seen.size+' / 36';});
document.querySelector('#scale').onchange=e=>{canvas.setAttribute('width',WIDTH*Number(e.target.value));canvas.setAttribute('height',HEIGHT*Number(e.target.value));};
document.querySelector('#reset').onclick=()=>{seen.clear();log.textContent='';document.querySelector('#count').textContent='0 / 36';};
</script></html>'''.replace('WIDTH',str(image.width)).replace('HEIGHT',str(image.height)).replace('LINKS',links)
    (destination/'acceptance.html').write_text(html,encoding='utf-8')
    print(f'Created {len(words)} word links and templates in {destination}')

if __name__=='__main__': main()
