### ppsspp_simple_ctf_loader

This is a port of https://github.com/ErikPshat/cxmb for PPSSPP's XMB mode, it implements the following features:

- extract ctf theme
- overlay XMB resources from extracted ctf theme
- hot patch scePaf_Module, sceVshCommonGui_Module and vsh_module

#### Usage

1. Extract `PSP` directory to PPSSPP's memory stick, 
2. Install theme as `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/theme.ctf`, make sure your theme's firmware matches your installed PSP firmware, otherwise themes that supplies prx-es and patches to scePaf_Module, sceVshCommonGui_Module and vsh_module would likely crash
3. Enable plugins at `Settings -> System -> Cheats -> Enable plugins`
4. Boot XMB
