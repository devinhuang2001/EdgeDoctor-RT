"""Compile production LVGL UI on host and render four pages without hardware."""
from pathlib import Path
import argparse, concurrent.futures, os, shutil, subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--bsp', default=r'D:\CODEX SPACE\infei\edgi-workspace\Edgi_Talk_M55_EdgeDoctor')
args = parser.parse_args()
bsp = Path(args.bsp)
lvgl = bsp / 'libraries/components/lvgl_9.2.0'
out = root / 'build/ui-preview'
out.mkdir(parents=True, exist_ok=True)
gcc = shutil.which('gcc')
if not gcc: raise SystemExit('Host gcc is required')
includes = [Path(__file__).parent, lvgl, root/'firmware/common/include']
includes += [root/'firmware/m55'/p for p in ['ui','runtime','logger','protocol']]
includes += sorted({p.parent for p in (lvgl/'src').rglob('*.h')})
flags = ['-std=c99','-O1','-g','-DED_UI_HOST_PREVIEW','-DLV_CONF_SKIP','-DLV_USE_FLOAT=1','-DLV_USE_TINY_TTF=1','-DLV_MEM_SIZE=16777216','-DLV_COLOR_DEPTH=32']
flags += ['-I'+str(p) for p in includes]
sources = list((lvgl/'src').rglob('*.c'))
sources += [Path(__file__).with_name('preview.c'),root/'firmware/m55/ui/edgedoctor_machine_image.c',bsp/'applications/xiaozhi/ui/xiaozhi_font.c']
sources += [bsp/'libraries/Common/board/ports/lvgl/lv_refr.c']
def compile_one(item):
    i,src=item
    obj=out/f'{i:04d}-{src.stem}.o'
    # Preview includes production C, so always rebuild this translation unit.
    if src.name!='preview.c' and obj.exists() and obj.stat().st_mtime>max(src.stat().st_mtime,Path(__file__).stat().st_mtime):return obj
    result=subprocess.run([gcc,*flags,'-c',str(src),'-o',str(obj)],capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stderr)
    if result.stderr:print(result.stderr)
    return obj
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    objects=list(pool.map(compile_one,enumerate(sources)))
response=out/'objects.rsp'
response.write_text('\n'.join('"'+str(p).replace('\\','/')+'"' for p in objects))
exe=out/'preview.exe'
subprocess.run([gcc,'@'+str(response),'-lm','-o',str(exe)],check=True)
run=subprocess.run([str(exe),str(out)],capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'layout.txt').write_text(run.stdout+run.stderr,encoding='utf-8')
run.check_returncode()
try:
    from PIL import Image
    for image in out.glob('tab-*.ppm'):Image.open(image).save(image.with_suffix('.png'))
except ImportError:print('Pillow unavailable; previews retained as PPM')
print(out)
