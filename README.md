### ppsspp_simple_ctf_loader

This is a port of https://github.com/ErikPshat/cxmb / https://github.com/PSP-Archive/CXMB for PPSSPP's XMB mode, it implements the following features:

- extract ctf theme
- overlay XMB resources from extracted ctf theme
- hot patch scePaf_Module, sceVshCommonGui_Module and vsh_module
- selecting themes from `<PPSSPP memory stick>/PSP/THEME` on FW 6.61

#### Plugin Installation

1. Extract `PSP` directory to PPSSPP's memory stick, 
2. Enable plugins at `Settings -> System -> Cheats -> Enable plugins`

#### Theme installation on FW 5.00+

1. Create directory `<PPSSPP memory stick>/PSP/THEME`
2. Install firmware compatible ctf themes files to `<PPSSPP memory stick>/PSP/THEME`, note that incompatible themes that supply prx-es and patches to scePaf_Module, sceVshCommonGui_Module and vsh_module would likely cause crashes, no strict checking is done currently when extracting themes, but version is logged to `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/log.txt`
3. Boot XMB and select your theme in `Settings -> Theme Settings -> Theme`
4. Once a theme is applied, XMB will quit shortly, theme will be applied after launching XMB again
5. If XMB hangs/crash, the theme is incompatible with your firmware, remove your theme [manually](#manual-theme-removal)

#### Theme removal on FW 5.00+

1. Boot XMB
2. Apply original/classic theme in `Settings -> Theme Settings -> Theme`
3. Open the theme list once again and delete the installed custom theme right under the official cookie theme
4. XMB will quit shortly, theme will be removed after launching XMB again

#### Manual theme installation

1. Install theme as `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/theme.ctf`, make sure your theme's firmware matches your installed PSP firmware, otherwise themes that supply prx-es and patches to scePaf_Module, sceVshCommonGui_Module and vsh_module would likely crash
2. Boot XMB

#### Manual theme removal

1. Remove `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/theme.ctf` and `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/active_theme`

#### Manual theme remixing

1. Files under `<PPSSPP memory stick>/PSP/PLUGINS/ppsspp_simple_ctf_loader/active_theme` can be modified, and will persist until the next theme installation/removal
