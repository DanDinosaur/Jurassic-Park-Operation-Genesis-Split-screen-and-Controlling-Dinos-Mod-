# Building dinput8.dll

Cross-compiled with **llvm-mingw** (tested: release 20251104, `ucrt-ubuntu-22.04-x86_64`) from
https://github.com/mstorsjo/llvm-mingw/releases (Linux or WSL; Windows llvm-mingw builds work the same way).

```
i686-w64-mingw32-clang++ -O2 -std=c++17 -Wall -Wno-unknown-pragmas \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DMINGW_HAS_SECURE_API \
  -static -static-libgcc -static-libstdc++ -shared -Wl,--kill-at -o dinput8.dll \
  src/*.cpp -lxinput9_1_0 -luser32 -lshlwapi -lkernel32
```

Check the result:

```
llvm-readobj --coff-exports dinput8.dll   # DirectInput8Create, DllCanUnloadNow, DllGetClassObject,
                                          # DllRegisterServer, DllUnregisterServer, GetdfDIJoystick, JPOGMod_GetHudRect
llvm-readobj --coff-imports dinput8.dll | grep -i xinput   # xinput9_1_0.dll only
```

Notes
- 32-bit (i686) only - the game is 32-bit.
- `-lxinput9_1_0`, not `-lxinput` (XINPUT1_3.dll is not on every Windows).
- `proxy.cpp` loads the real system dinput8.dll at run time and forwards to it; `dinput8.def` is `LIBRARY dinput8` only.
- Engine addresses in the source are for the retail PC `SimJP.exe` (5,775,360 bytes).
- Put the built `dinput8.dll` in `installer/files/` and run `installer/install.bat`.
