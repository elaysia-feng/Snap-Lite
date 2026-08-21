# Screenshot toolbar interaction notes

This branch follows the interaction pattern used by mainstream Tencent/QQ-style screenshot tools:

- keep annotation tools next to the selected region;
- show a secondary property bar only for the active tool;
- expose color plus width/font size instead of hard-coding annotation styles;
- make pin/save/copy completion actions discoverable in the same toolbar;
- keep global clipboard pinning as an additional power-user path.

The implementation intentionally keeps Snap-Lite's native Win32/GDI+ stack and dark floating chrome instead of copying another product's visual assets.
