"""Shared pyghidra session for the native-reconstruction scripts.

Opens the existing Ghidra project E:\\tools\\ghidra-projects\\SecondSon (program /eboot)
read-only, so a script never changes the analysis database. Only one process may hold the
project, so close the Ghidra GUI / pyghidra-mcp before running these scripts.

Run with the pyghidra-mcp virtualenv:
  set GHIDRA_INSTALL_DIR=E:\\tools\\ghidra & set JAVA_HOME=E:\\tools\\jdk21
  E:\\tools\\ghidra-mcp\\.venv\\Scripts\\python.exe -X utf8 <script>.py [args]
"""
import contextlib
import os

PROJECT_DIR = os.environ.get("SSPC_GHIDRA_PROJECT_DIR", r"E:\tools\ghidra-projects")
PROJECT_NAME = os.environ.get("SSPC_GHIDRA_PROJECT", "SecondSon")
PROGRAM = os.environ.get("SSPC_GHIDRA_PROGRAM", "eboot")


@contextlib.contextmanager
def program(read_only=True):
    import pyghidra
    pyghidra.start()
    from ghidra.base.project import GhidraProject
    proj = GhidraProject.openProject(PROJECT_DIR, PROJECT_NAME, True)
    prog = proj.openProgram("/", PROGRAM, read_only)
    try:
        yield prog
        if not read_only:
            proj.save(prog)
    finally:
        proj.close()


def decompiler(prog, timeout_s=60):
    from ghidra.app.decompiler import DecompInterface, DecompileOptions
    from ghidra.util.task import ConsoleTaskMonitor
    di = DecompInterface()
    di.setOptions(DecompileOptions())
    di.openProgram(prog)
    mon = ConsoleTaskMonitor()

    def decompile(func):
        res = di.decompileFunction(func, timeout_s, mon)
        if res is None or not res.decompileCompleted():
            return None
        return res.getDecompiledFunction().getC()
    return decompile


def addr(prog, value):
    return prog.getAddressFactory().getDefaultAddressSpace().getAddress(value)
