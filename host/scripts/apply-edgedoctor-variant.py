"""Apply build-only EdgeDoctor exclusions to an integration BSP copy."""
import argparse
from pathlib import Path


def apply(project: Path) -> None:
    wake = project / "applications/xiaozhi/wake_word/SConscript"
    edge = project / "edge-impulse/SConscript"
    if not wake.is_file() or not edge.is_file():
        raise ValueError("Expected XiaoZhi wake_word and Edge Impulse build scripts")
    wake.write_text("# EdgeDoctor supplies explicit disabled ABI stubs.\n"
                    "group = []\nReturn('group')\n", encoding="utf-8")
    edge.write_text('''# EdgeDoctor needs CMSIS-DSP, not the wake-word inference runtime.
from building import *
import os

cwd = GetCurrentDir()
cmsis = os.path.join(cwd, 'edge-impulse-sdk', 'CMSIS')
src = []
for category in ('TransformFunctions', 'CommonTables', 'BasicMathFunctions',
                 'FastMathFunctions', 'StatisticsFunctions', 'SupportFunctions',
                 'MatrixFunctions', 'ComplexMathFunctions'):
    src += Glob(os.path.join(cmsis, 'DSP', 'Source', category, '*.c'))
group = DefineGroup('edgedoctor_cmsis_dsp', src, depend=[],
    CPPPATH=[os.path.join(cmsis, 'DSP', 'Include'),
             os.path.join(cmsis, 'DSP', 'PrivateInclude'),
             os.path.join(cmsis, 'Core', 'Include')],
    LOCAL_CFLAGS=' -DARM_MATH_CM55 -DARM_MATH_DSP -DARM_MATH_AUTOVECTORIZE')
Return('group')
''', encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    apply(parser.parse_args().project.resolve())
